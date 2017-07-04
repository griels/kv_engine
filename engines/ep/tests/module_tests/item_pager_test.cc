/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

/**
 * Unit tests for Item Paging / Expiration.
 */

#include "ep_time.h"
#include "evp_store_single_threaded_test.h"
#include "test_helpers.h"
#include "tests/mock/mock_synchronous_ep_engine.h"

#include <gtest/gtest.h>

/**
 * Test fixture for KVBucket tests running in single-threaded mode.
 *
 * Parameterised on the bucket_type (i.e. Ephemeral or Peristent).
 */
class STParameterizedBucketTest
        : public SingleThreadedEPBucketTest,
          public ::testing::WithParamInterface<std::string> {
protected:
    void SetUp() override;
};

void STParameterizedBucketTest::SetUp() {
    if (!config_string.empty()) {
        config_string += ";";
    }
    config_string += "bucket_type=" + GetParam();
    SingleThreadedEPBucketTest::SetUp();
}

/**
 * Test fixture for bucket quota tests. Sets quota (max_size) to 128KB and
 * enables the MemoryTracker.
 *
 * NOTE: All the tests using this (including subclasses) require memory
 * tracking to be enabled.
 */
class STBucketQuotaTest : public STParameterizedBucketTest {
public:
    static void SetUpTestCase() {
        // Setup the MemoryTracker.
        MemoryTracker::getInstance(*get_mock_server_api()->alloc_hooks);
    }

    static void TearDownTestCase() {
        MemoryTracker::destroyInstance();
    }

protected:
    void SetUp() override {
        // Set specific ht_size given we need to control expected memory usage.
        config_string += "ht_size=47;max_size=" + std::to_string(200 * 1024) +
                         ";mem_low_wat=" + std::to_string(120 * 1024) +
                         ";mem_high_wat=" + std::to_string(160 * 1024);
        STParameterizedBucketTest::SetUp();

        // How many nonIO tasks we expect initially
        // - 0 for persistent.
        // - 1 for Ephemeral (EphTombstoneHTCleaner).
        if (GetParam() == "ephemeral") {
            ++initialNonIoTasks;
        }

        // Sanity check - need memory tracker to be able to check our memory
        // usage.
        ASSERT_TRUE(MemoryTracker::trackingMemoryAllocations())
            << "Memory tracker not enabled - cannot continue";

        store->setVBucketState(vbid, vbucket_state_active, false);

        // Sanity check - to ensure memory usage doesn't increase without us
        // noticing.
        ASSERT_EQ(47, store->getVBucket(vbid)->ht.getSize())
                << "Expected to have a HashTable of size 47 (mem calculations "
                   "based on this).";
        auto& stats = engine->getEpStats();
        ASSERT_LE(stats.getTotalMemoryUsed(), 20 * 1024)
            << "Expected to start with less than 20KB of memory used";
        ASSERT_LT(stats.getTotalMemoryUsed(), stats.getMaxDataSize() * 0.5)
            << "Expected to start below 50% of bucket quota";
    }

    ENGINE_ERROR_CODE storeItem(Item& item) {
        uint64_t cas = 0;
        return engine->store(nullptr, &item, &cas, OPERATION_SET);
    }

    /**
     * Write documents to the bucket until they fail with TMP_FAIL.
     * Note this stores via external API (epstore) so we trigger the
     * memoryCondition() code in the event of ENGINE_ENOMEM.
     *
     * @param vbid vBucket to write items to.
     * @param expiry value for items. 0 == no TTL.
     * @return number of documents written.
     */
    size_t populateUntilTmpFail(uint16_t vbid, rel_time_t ttl = 0) {
        size_t count = 0;
        const std::string value(512, 'x'); // 512B value to use for documents.
        ENGINE_ERROR_CODE result;
        const auto expiry = (ttl != 0) ? ep_abs_time(ttl) : time_t(0);
        for (result = ENGINE_SUCCESS; result == ENGINE_SUCCESS; count++) {
            auto key = makeStoredDocKey("xxx_" + std::to_string(count));
            auto item = make_item(vbid, key, value, expiry);
            // Set NRU of item to maximum; so will be a candidate for paging out
            // straight away.
            item.setNRUValue(MAX_NRU_VALUE);
            result = storeItem(item);
        }
        EXPECT_EQ(ENGINE_TMPFAIL, result);
        // Fixup count for last loop iteration.
        --count;

        auto& stats = engine->getEpStats();
        EXPECT_GT(stats.getTotalMemoryUsed(), stats.getMaxDataSize() * 0.8)
            << "Expected to exceed 80% of bucket quota after hitting TMPFAIL";
        EXPECT_GT(stats.getTotalMemoryUsed(), stats.mem_low_wat.load())
            << "Expected to exceed low watermark after hitting TMPFAIL";

        // To ensure the Blobs can actually be removed from memory, they must have
        // a ref-count of 1. This will not be the case if there's any open
        // checkpoints hanging onto Items. Therefore force the creation of a new
        // checkpoint.
        store->getVBucket(vbid)->checkpointManager.createNewCheckpoint();

        // Ensure items are flushed to disk (so we can evict them).
        if (GetParam() == "persistent") {
            store->flushVBucket(vbid);
        }

        return count;
    }

    /// Count of nonIO tasks we should initially have.
    size_t initialNonIoTasks = 0;
};

/**
 * Test fixture for item pager tests - enables the Item Pager (in addition to
 * what the parent class does).
 */
class STItemPagerTest : public STBucketQuotaTest {
protected:
    void SetUp() override {
        STBucketQuotaTest::SetUp();
        scheduleItemPager();
        ++initialNonIoTasks;

        // Sanity check - should be no nonIO tasks ready to run, and one in
        // futureQ (ItemPager).
        auto& lpNonioQ = *task_executor->getLpTaskQ()[NONIO_TASK_IDX];
        EXPECT_EQ(0, lpNonioQ.getReadyQueueSize());
        EXPECT_EQ(initialNonIoTasks, lpNonioQ.getFutureQueueSize());

        // We shouldn't be able to schedule the Item Pager task yet as it's not
        // ready.
        try {
            SCOPED_TRACE("");
            runNextTask(lpNonioQ, "Paging out items.");
            FAIL() << "Unexpectedly managed to run Item Pager";
        } catch (std::logic_error&) {
        }
    }

    void runItemPager(size_t online_vb_count = 1) {
        auto& lpNonioQ = *task_executor->getLpTaskQ()[NONIO_TASK_IDX];

        // Item pager consists of two Tasks - the parent ItemPager task,
        // and then a per-vBucket task (via VCVBAdapter) - which there is
        // just one of as we only have one vBucket online.
        ASSERT_EQ(0, lpNonioQ.getReadyQueueSize());
        ASSERT_EQ(initialNonIoTasks, lpNonioQ.getFutureQueueSize());
        runNextTask(lpNonioQ, "Paging out items.");
        ASSERT_EQ(0, lpNonioQ.getReadyQueueSize());
        ASSERT_EQ(initialNonIoTasks + 1, lpNonioQ.getFutureQueueSize());
        for (size_t ii = 0; ii < online_vb_count; ii++) {
            runNextTask(lpNonioQ, "Item pager on vb 0");
        }
        // Once complete, should have the same number of tasks we initially
        // had.
        ASSERT_EQ(0, lpNonioQ.getReadyQueueSize());
        ASSERT_EQ(initialNonIoTasks, lpNonioQ.getFutureQueueSize());
    }
};

// Test that the ItemPager is scheduled when the Server Quota is reached, and
// that items are successfully paged out.
TEST_P(STItemPagerTest, ServerQuotaReached) {

    size_t count = populateUntilTmpFail(vbid);
    ASSERT_GE(count, 50) << "Too few documents stored";

    runItemPager();

    auto& stats = engine->getEpStats();
    EXPECT_LT(stats.getTotalMemoryUsed(), stats.mem_low_wat.load())
            << "Expected to be below low watermark after running item pager";
}

// Test that when the server quota is reached, we delete items which have
// expired before any other items.
TEST_P(STItemPagerTest, ExpiredItemsDeletedFirst) {

    // Populate bucket with non-expiring items until we reach the low
    // watermark.
    size_t countA = 0;
    const std::string value(512, 'x'); // 512B value to use for documents.
    auto& stats = engine->getEpStats();
    do {
        auto key = makeStoredDocKey("key_" + std::to_string(countA));
        auto item = make_item(vbid, key, value);
        ASSERT_EQ(ENGINE_SUCCESS, storeItem(item));
        countA++;
    } while (stats.getTotalMemoryUsed() < stats.mem_low_wat.load());

    ASSERT_GE(countA, 10)
            << "Expected at least 10 items before hitting low watermark";

    // Fill bucket with items with a TTL of 1s until we hit ENOMEM. When
    // we run the pager, we expect these items to be deleted first.
    auto countB = populateUntilTmpFail(vbid, 1);

    ASSERT_GE(countB, 50)
        << "Expected at least 50 documents total before hitting high watermark";

    // Advance time so when the pager runs it will find expired items.
    TimeTraveller billSPrestonEsq(2);

    runItemPager();

    // Check which items remain. We should have deleted all of the items with
    // a TTL, as they should have been considered first).

    // Initial documents should still exist:
    for (size_t ii = 0; ii < countA; ii++) {
        auto key = makeStoredDocKey("key_" + std::to_string(ii));
        auto result = store->get(key, vbid, nullptr, get_options_t());
        EXPECT_EQ(ENGINE_SUCCESS, result.getStatus()) << "For key:" << key;
    }

    // Documents which had a TTL should be deleted:
    for (size_t ii = 0; ii < countB; ii++) {
        auto key = makeStoredDocKey("xxx_" + std::to_string(ii));
        auto result = store->get(key, vbid, nullptr, get_options_t());
        EXPECT_EQ(ENGINE_KEY_ENOENT, result.getStatus()) << "For key:" << key;
    }
}

/**
 * Test fixture for Ephemeral-only item pager tests.
 */
class STEphemeralItemPagerTest : public STItemPagerTest {
};

// For Ephemeral buckets, replica items should not be paged out (deleted) -
// as that would cause the replica to have a diverging history from the active.
TEST_P(STEphemeralItemPagerTest, ReplicaNotPaged) {
    const uint16_t active_vb = 0;
    const uint16_t replica_vb = 1;
    // Set vBucket 1 online, initially as active (so we can populate it).
    store->setVBucketState(replica_vb, vbucket_state_active, false);

    auto& stats = engine->getEpStats();
    ASSERT_LE(stats.getTotalMemoryUsed(), 40 * 1024)
        << "Expected to start with less than 40KB of memory used";
    ASSERT_LT(stats.getTotalMemoryUsed(), stats.mem_low_wat.load())
        << "Expected to start below low watermark";

    // Populate vbid 0 (active) until we reach the low watermark.
    size_t active_count = 0;
    const std::string value(1024, 'x'); // 1KB value to use for documents.
    do {
        auto key = makeStoredDocKey("key_" + std::to_string(active_count));
        auto item = make_item(active_vb, key, value);
        // Set NRU of item to maximum; so will be a candidate for paging out
        // straight away.
        item.setNRUValue(MAX_NRU_VALUE);
        ASSERT_EQ(ENGINE_SUCCESS, storeItem(item));
        active_count++;
    } while (stats.getTotalMemoryUsed() < stats.mem_low_wat.load());

    ASSERT_GE(active_count, 10)
            << "Expected at least 10 active items before hitting low watermark";

    // Populate vbid 1 (replica) until we reach the high watermark.
    size_t replica_count = populateUntilTmpFail(replica_vb);
    ASSERT_GE(replica_count, 10)
        << "Expected at least 10 replica items before hitting high watermark";

    // Flip vb 1 to be a replica (and hence should not be a candidate for
    // any paging out.
    store->setVBucketState(replica_vb, vbucket_state_replica, false);

    runItemPager(2);

    EXPECT_EQ(replica_count, store->getVBucket(replica_vb)->getNumItems())
        << "Replica count should be unchanged after Item Pager";

    EXPECT_LT(stats.getTotalMemoryUsed(), stats.mem_low_wat.load())
            << "Expected to be below low watermark after running item pager";

    EXPECT_LT(store->getVBucket(active_vb)->getNumItems(), active_count)
        << "Active count should have decreased after Item Pager";
}

/**
 * Test fixture for expiry pager tests - enables the Expiry Pager (in addition
 * to what the parent class does).
 */
class STExpiryPagerTest : public STBucketQuotaTest {
protected:
    void SetUp() override {
        STBucketQuotaTest::SetUp();

        // Setup expiry pager - this adds one to the number of nonIO tasks
        initializeExpiryPager();
        ++initialNonIoTasks;

        // Sanity check - should be no nonIO tasks ready to run, and initial
        // count in futureQ.
        auto& lpNonioQ = *task_executor->getLpTaskQ()[NONIO_TASK_IDX];
        EXPECT_EQ(0, lpNonioQ.getReadyQueueSize());
        EXPECT_EQ(initialNonIoTasks, lpNonioQ.getFutureQueueSize());
    }

    void runExpiryPager() {
        store->disableExpiryPager();
        store->enableExpiryPager();
        // Expiry pager consists of two Tasks - the parent ExpiryPager task,
        // and then a per-vBucket task (via VCVBAdapter) - which there is
        // just one of as we only have one vBucket online.
        // Trigger expiry pager - note the main task just spawns individual
        // tasks per vBucket - we also need to execute one of them.
        auto& lpNonioQ = *task_executor->getLpTaskQ()[NONIO_TASK_IDX];
        runNextTask(lpNonioQ, "Paging expired items.");
        EXPECT_EQ(0, lpNonioQ.getReadyQueueSize());
        EXPECT_EQ(initialNonIoTasks + 1, lpNonioQ.getFutureQueueSize());
        runNextTask(lpNonioQ, "Expired item remover on vb 0");
        EXPECT_EQ(0, lpNonioQ.getReadyQueueSize());
        EXPECT_EQ(initialNonIoTasks, lpNonioQ.getFutureQueueSize());
    }
};

// Test that when the expiry pager runs, all expired items are deleted.
TEST_P(STExpiryPagerTest, ExpiredItemsDeleted) {

    // Populate bucket with three documents - one with no expiry, one with an
    // expiry in 10 seconds, and one with an expiry in 20 seconds.
    const std::string value(512, 'x'); // 512B value to use for documents.
    for (size_t ii = 0; ii < 3; ii++) {
        auto key = makeStoredDocKey("key_" + std::to_string(ii));
        const uint32_t expiry =
                ii > 0 ? ep_abs_time(ep_current_time() + ii * 10) : 0;
        auto item = make_item(vbid, key, value, expiry);
        ASSERT_EQ(ENGINE_SUCCESS, storeItem(item));
    }

    if (GetParam() == "persistent") {
        EXPECT_EQ(3, store->flushVBucket(vbid));
    }

    // Sanity check - should have not hit high watermark (otherwise the
    // item pager will run automatically and aggressively delete items).
    auto& stats = engine->getEpStats();
    EXPECT_LE(stats.getTotalMemoryUsed(), stats.getMaxDataSize() * 0.8)
        << "Expected to not have exceeded 80% of bucket quota";

    // Move time forward by 11s, so key_1 should be expired.
    TimeTraveller tedTheodoreLogan(11);

    // Sanity check - should still have all items present in VBucket.
    ASSERT_EQ(3, engine->getVBucket(vbid)->getNumItems());

    runExpiryPager();
    if (GetParam() == "persistent") {
        EXPECT_EQ(1, store->flushVBucket(vbid));
    }

    EXPECT_EQ(2, engine->getVBucket(vbid)->getNumItems())
        << "Should only have 2 items after running expiry pager";

    // Check our items.
    auto key_0 = makeStoredDocKey("key_0");
    auto result = store->get(key_0, vbid, nullptr, get_options_t());
    EXPECT_EQ(ENGINE_SUCCESS, result.getStatus())
        << "Key without TTL should still exist.";

    auto key_1 = makeStoredDocKey("key_1");
    EXPECT_EQ(ENGINE_KEY_ENOENT,
              store->get(key_1, vbid, nullptr, get_options_t()).getStatus())
        << "Key with TTL:10 should be removed.";

    auto key_2 = makeStoredDocKey("key_2");
    result = store->get(key_2, vbid, nullptr, get_options_t());
    EXPECT_EQ(ENGINE_SUCCESS, result.getStatus())
         << "Key with TTL:20 should still exist.";

    // Move time forward by +10s, so key_2 should also be expired.
    TimeTraveller philConners(10);

    // Sanity check - should still have 2 items present in VBucket.
    ASSERT_EQ(2, engine->getVBucket(vbid)->getNumItems())
        << "Should still have 2 items after time-travelling";

    runExpiryPager();
    if (GetParam() == "persistent") {
        EXPECT_EQ(1, store->flushVBucket(vbid));
    }

    // Should only be 1 item remaining.
    EXPECT_EQ(1, engine->getVBucket(vbid)->getNumItems());

    // Check our items.
    result = store->get(key_0, vbid, nullptr, get_options_t());
    EXPECT_EQ(ENGINE_SUCCESS, result.getStatus())
        << "Key without TTL should still exist.";

    EXPECT_EQ(ENGINE_KEY_ENOENT,
              store->get(key_1, vbid, nullptr, get_options_t()).getStatus())
        << "Key with TTL:10 should be removed.";

    EXPECT_EQ(ENGINE_KEY_ENOENT,
              store->get(key_2, vbid, nullptr, get_options_t()).getStatus())
    << "Key with TTL:20 should be removed.";
}

// TODO: Ideally all of these tests should run with or without jemalloc,
// however we currently rely on jemalloc for accurate memory tracking; and
// hence it is required currently.
#if defined(HAVE_JEMALLOC)
INSTANTIATE_TEST_CASE_P(EphemeralOrPersistent,
                        STItemPagerTest,
                        ::testing::Values("ephemeral", "persistent"),
                        [](const ::testing::TestParamInfo<std::string>& info) {
                            return info.param;
                        });

INSTANTIATE_TEST_CASE_P(EphemeralOrPersistent,
                        STExpiryPagerTest,
                        ::testing::Values("ephemeral", "persistent"),
                        [](const ::testing::TestParamInfo<std::string>& info) {
                            return info.param;
                        });

INSTANTIATE_TEST_CASE_P(Ephemeral,
                        STEphemeralItemPagerTest,
                        ::testing::Values("ephemeral"),
                        [](const ::testing::TestParamInfo<std::string>& info) {
                            return info.param;
                        });
#endif
