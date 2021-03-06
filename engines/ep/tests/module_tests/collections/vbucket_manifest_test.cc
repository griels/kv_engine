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

#include "checkpoint.h"
#include "collections/manifest.h"
#include "collections/vbucket_manifest.h"
#include "collections/vbucket_serialised_manifest_entry.h"
#include "ep_vb.h"
#include "failover-table.h"
#include "tests/module_tests/test_helpers.h"

#include <cJSON_utils.h>

#include <gtest/gtest.h>

class MockVBManifest : public Collections::VB::Manifest {
public:
    MockVBManifest() : Collections::VB::Manifest({/* no collection data*/}) {
    }

    MockVBManifest(const std::string& json) : Collections::VB::Manifest(json) {
    }

    bool exists(Collections::Identifier identifier) const {
        std::lock_guard<cb::ReaderLock> readLock(rwlock.reader());
        return exists_UNLOCKED(identifier);
    }

    bool isOpen(Collections::Identifier identifier) const {
        std::lock_guard<cb::ReaderLock> readLock(rwlock.reader());
        expect_true(exists_UNLOCKED(identifier));
        auto itr = map.find(identifier.getName());
        return itr->second->isOpen();
    }

    bool isExclusiveOpen(Collections::Identifier identifier) const {
        std::lock_guard<cb::ReaderLock> readLock(rwlock.reader());
        expect_true(exists_UNLOCKED(identifier));
        auto itr = map.find(identifier.getName());
        return itr->second->isExclusiveOpen();
    }

    bool isDeleting(Collections::Identifier identifier) const {
        std::lock_guard<cb::ReaderLock> readLock(rwlock.reader());
        expect_true(exists_UNLOCKED(identifier));
        auto itr = map.find(identifier.getName());
        return itr->second->isDeleting();
    }

    bool isExclusiveDeleting(Collections::Identifier identifier) const {
        std::lock_guard<cb::ReaderLock> readLock(rwlock.reader());
        expect_true(exists_UNLOCKED(identifier));
        auto itr = map.find(identifier.getName());
        return itr->second->isExclusiveDeleting();
    }

    bool isOpenAndDeleting(Collections::Identifier identifier) const {
        std::lock_guard<cb::ReaderLock> readLock(rwlock.reader());
        expect_true(exists_UNLOCKED(identifier));
        auto itr = map.find(identifier.getName());
        return itr->second->isOpenAndDeleting();
    }

    size_t size() const {
        std::lock_guard<cb::ReaderLock> readLock(rwlock.reader());
        return map.size();
    }

    bool compareEntry(const Collections::VB::ManifestEntry& entry) const {
        std::lock_guard<cb::ReaderLock> readLock(rwlock.reader());
        if (exists_UNLOCKED(entry.getIdentifier())) {
            auto itr = map.find(entry.getCollectionName());
            const auto& myEntry = *itr->second;
            return myEntry.getStartSeqno() == entry.getStartSeqno() &&
                   myEntry.getEndSeqno() == entry.getEndSeqno() &&
                   myEntry.getUid() == entry.getUid();
        }
        return false;
    }

    bool operator==(const MockVBManifest& rhs) const {
        std::lock_guard<cb::ReaderLock> readLock(rwlock.reader());
        if (rhs.size() != size()) {
            return false;
        }
        // Check all collections match
        for (const auto& e : map) {
            if (!rhs.compareEntry(*e.second)) {
                return false;
            }
        }

        // finally check the separator's match
        return rhs.separator == separator;
    }

    bool operator!=(const MockVBManifest& rhs) const {
        return !(*this == rhs);
    }

    int64_t getGreatestEndSeqno() const {
        std::lock_guard<cb::ReaderLock> readLock(rwlock.reader());
        return greatestEndSeqno;
    }

    size_t getNumDeletingCollections() const {
        std::lock_guard<cb::ReaderLock> readLock(rwlock.reader());
        return nDeletingCollections;
    }

    bool isGreatestEndSeqnoCorrect() const {
        std::lock_guard<cb::ReaderLock> readLock(rwlock.reader());
        // If this is zero greatestEnd should not be a seqno
        if (nDeletingCollections == 0) {
            return greatestEndSeqno == StoredValue::state_collection_open;
        }
        return greatestEndSeqno >= 0;
    }

    bool isNumDeletingCollectionsoCorrect() const {
        std::lock_guard<cb::ReaderLock> readLock(rwlock.reader());
        // If this is zero greatestEnd should not be a seqno
        if (greatestEndSeqno != StoredValue::state_collection_open) {
            return nDeletingCollections > 0;
        }
        return nDeletingCollections == 0;
    }

protected:
    bool exists_UNLOCKED(Collections::Identifier identifier) const {
        auto itr = map.find(identifier.getName());
        return itr != map.end() && itr->second->getUid() == identifier.getUid();
    }

    void expect_true(bool in) const {
        if (!in) {
            std::stringstream ss;
            ss << *this;
            throw std::logic_error("expect_true found false manifest:" +
                                   ss.str());
        }
    }
};

/**
 * Test class that owns an active and replica manifest.
 * Updates applied to the active are applied to the replica by processing
 * the active's checkpoint.
 */
class ActiveReplicaManifest {
public:
    /// Dummy callback to replace the flusher callback so we can create VBuckets
    class DummyCB : public Callback<uint16_t> {
    public:
        DummyCB() {
        }

        void callback(uint16_t& dummy) {
        }
    };

    ActiveReplicaManifest()
        : active(),
          replica(),
          vbA(0,
              vbucket_state_active,
              global_stats,
              checkpoint_config,
              /*kvshard*/ nullptr,
              /*lastSeqno*/ 0,
              /*lastSnapStart*/ 0,
              /*lastSnapEnd*/ 0,
              /*table*/ nullptr,
              std::make_shared<DummyCB>(),
              /*newSeqnoCb*/ nullptr,
              config,
              VALUE_ONLY),
          vbR(1,
              vbucket_state_replica,
              global_stats,
              checkpoint_config,
              /*kvshard*/ nullptr,
              /*lastSeqno*/ 0,
              /*lastSnapStart*/ 0,
              /*lastSnapEnd*/ snapEnd,
              /*table*/ nullptr,
              std::make_shared<DummyCB>(),
              /*newSeqnoCb*/ nullptr,
              config,
              VALUE_ONLY),
          lastCompleteDeletionArgs({}, 0) {
    }

    ::testing::AssertionResult update(const char* json) {
        try {
            active.wlock().update(vbA, {json});
        } catch (std::exception& e) {
            return ::testing::AssertionFailure()
                   << "Exception thrown for update with " << json
                   << ", e.what:" << e.what();
        }
        queued_item manifest;
        try {
            manifest = applyCheckpointEventsToReplica();
        } catch (std::exception& e) {
            return ::testing::AssertionFailure()
                   << "Exception thrown for replica update, e.what:"
                   << e.what();
        }
        if (active != replica) {
            return ::testing::AssertionFailure()
                   << "active doesn't match replica active:\n"
                   << active << " replica:\n"
                   << replica;
        }

        auto rv = checkNumDeletingCollections();
        if (rv != ::testing::AssertionSuccess()) {
            return rv;
        }
        rv = checkGreatestEndSeqno();
        if (rv != ::testing::AssertionSuccess()) {
            return rv;
        }

        return checkJson(*manifest);
    }

    ::testing::AssertionResult completeDeletion(
            Collections::Identifier identifier) {
        try {
            active.wlock().completeDeletion(vbA, identifier.getName());
            lastCompleteDeletionArgs = identifier;
        } catch (std::exception& e) {
            return ::testing::AssertionFailure()
                   << "Exception thrown for completeDeletion with e.what:"
                   << e.what();
        }

        queued_item manifest;
        try {
            manifest = applyCheckpointEventsToReplica();
        } catch (std::exception& e) {
            return ::testing::AssertionFailure()
                   << "completeDeletion: Exception thrown for replica update, "
                      "e.what:"
                   << e.what();
        }

        // completeDeletion adds a new item without a seqno, which closes
        // the snapshot, re-open the snapshot so tests can continue.
        vbR.checkpointManager->updateCurrentSnapshotEnd(snapEnd);
        if (active != replica) {
            return ::testing::AssertionFailure()
                   << "completeDeletion: active doesn't match replica active:\n"
                   << active << " replica:\n"
                   << replica;
        }
        return checkJson(*manifest);
    }

    ::testing::AssertionResult doesKeyContainValidCollection(DocKey key) {
        if (!active.lock().doesKeyContainValidCollection(key)) {
            return ::testing::AssertionFailure() << "active failed the key";
        } else if (!replica.lock().doesKeyContainValidCollection(key)) {
            return ::testing::AssertionFailure() << "replica failed the key";
        }
        return ::testing::AssertionSuccess();
    }

    ::testing::AssertionResult isLogicallyDeleted(DocKey key, int64_t seqno) {
        if (!active.lock().isLogicallyDeleted(key, seqno)) {
            return ::testing::AssertionFailure()
                   << "active failed the key seqno:" << seqno << "\n"
                   << active;
        } else if (!replica.lock().isLogicallyDeleted(key, seqno)) {
            return ::testing::AssertionFailure()
                   << "replica failed the key seqno:" << seqno << "\n"
                   << replica;
        }
        return ::testing::AssertionSuccess();
    }

    bool isOpen(Collections::Identifier identifier) {
        return active.isOpen(identifier) && replica.isOpen(identifier);
    }

    bool isDeleting(Collections::Identifier identifier) {
        return active.isDeleting(identifier) && replica.isDeleting(identifier);
    }

    bool isExclusiveOpen(Collections::Identifier identifier) {
        return active.isExclusiveOpen(identifier) &&
               replica.isExclusiveOpen(identifier);
    }

    bool isExclusiveDeleting(Collections::Identifier identifier) {
        return active.isExclusiveDeleting(identifier) &&
               replica.isExclusiveDeleting(identifier);
    }

    bool isOpenAndDeleting(Collections::Identifier identifier) {
        return active.isOpenAndDeleting(identifier) &&
               replica.isOpenAndDeleting(identifier);
    }

    bool checkSize(size_t s) {
        return active.size() == s && replica.size() == s;
    }

    VBucket& getActiveVB() {
        return vbA;
    }

    MockVBManifest& getActiveManifest() {
        return active;
    }

    int64_t getLastSeqno() const {
        return lastSeqno;
    }

    ::testing::AssertionResult checkGreatestEndSeqno(int64_t expectedSeqno) {
        if (active.getGreatestEndSeqno() != expectedSeqno) {
            return ::testing::AssertionFailure()
                   << "active failed expectedSeqno:" << expectedSeqno << "\n"
                   << active;
        } else if (replica.getGreatestEndSeqno() != expectedSeqno) {
            return ::testing::AssertionFailure()
                   << "replica failed expectedSeqno:" << expectedSeqno << "\n"
                   << replica;
        }
        return ::testing::AssertionSuccess();
    }

    ::testing::AssertionResult checkNumDeletingCollections(size_t expected) {
        if (active.getNumDeletingCollections() != expected) {
            return ::testing::AssertionFailure()
                   << "active failed expected:" << expected << "\n"
                   << active;
        } else if (replica.getNumDeletingCollections() != expected) {
            return ::testing::AssertionFailure()
                   << "replica failed expected:" << expected << "\n"
                   << replica;
        }
        return ::testing::AssertionSuccess();
    }

    ::testing::AssertionResult checkNumDeletingCollections() {
        if (!active.isNumDeletingCollectionsoCorrect()) {
            return ::testing::AssertionFailure()
                   << "checkNumDeletingCollections active failed " << active;
        } else if (!replica.isNumDeletingCollectionsoCorrect()) {
            return ::testing::AssertionFailure()
                   << "checkNumDeletingCollections replica failed " << replica;
        }
        return ::testing::AssertionSuccess();
    }

    ::testing::AssertionResult checkGreatestEndSeqno() {
        if (!active.isGreatestEndSeqnoCorrect()) {
            return ::testing::AssertionFailure()
                   << "checkGreatestEndSeqno active failed " << active;
        } else if (!replica.isGreatestEndSeqnoCorrect()) {
            return ::testing::AssertionFailure()
                   << "checkGreatestEndSeqno replica failed " << replica;
        }
        return ::testing::AssertionSuccess();
    }

private:

    static void getEventsFromCheckpoint(VBucket& vb,
                                        std::vector<queued_item>& events) {
        std::vector<queued_item> items;
        vb.checkpointManager->getAllItemsForCursor(
                CheckpointManager::pCursorName, items);
        for (const auto& qi : items) {
            if (qi->getOperation() == queue_op::system_event) {
                events.push_back(qi);
            }
        }

        if (events.empty()) {
            throw std::logic_error("getEventsFromCheckpoint: no events in vb:" +
                                   std::to_string(vb.getId()));
        }
    }

    /**
     * 1. scan the VBucketManifestTestVBucket's checkpoint for all system
     * events.
     * 2. for all system-events, pretend to be the DcpConsumer and call
     *    the VBucket's manifest's replica functions on.
     * @param replicaVB A vbucket acting as the replica, we will create/delete
     *        collections against this VB.
     * @param replicaManfiest The replica VB's manifest, we will create/delete
     *         collections against this manifest.
     *
     * @returns the last queued_item (which would be used to create a json
     *          manifest)
     */
    queued_item applyCheckpointEventsToReplica() {
        std::vector<queued_item> events;
        getEventsFromCheckpoint(vbA, events);
        queued_item rv = events.back();
        for (const auto& qi : events) {
            lastSeqno = qi->getBySeqno();
            if (qi->getOperation() == queue_op::system_event) {
                auto dcpData = Collections::VB::Manifest::getSystemEventData(
                        {qi->getData(), qi->getNBytes()});

                // Extract the revision to a local
                auto uid = *reinterpret_cast<const Collections::uid_t*>(
                        dcpData.second.data());

                switch (SystemEvent(qi->getFlags())) {
                case SystemEvent::Collection: {
                    if (qi->isDeleted()) {
                        // A deleted create means beginDelete collection
                        replica.wlock().replicaBeginDelete(
                                vbR, {dcpData.first, uid}, qi->getBySeqno());
                    } else {
                        replica.wlock().replicaAdd(
                                vbR, {dcpData.first, uid}, qi->getBySeqno());
                    }
                    break;
                }
                case SystemEvent::CollectionsSeparatorChanged: {
                    auto dcpData = Collections::VB::Manifest::
                            getSystemEventSeparatorData(
                                    {qi->getData(), qi->getNBytes()});
                    replica.wlock().replicaChangeSeparator(
                            vbR, dcpData, qi->getBySeqno());
                    break;
                }
                case SystemEvent::DeleteCollectionSoft:
                case SystemEvent::DeleteCollectionHard:
                    // DCP doesn't transmit these events, but to improve test
                    // coverage call completeDeletion on the replica only in
                    // response to these system events appearing in the
                    // checkpoint. The data held in the system event isn't
                    // suitable though for forming the arguments to the function
                    // e.g. Delete hard, the serialised manifest doesn't have
                    // the collection:rev we pass through, hence why we cache
                    // the collection:rev data in lastCompleteDeletionArgs
                    replica.wlock().completeDeletion(
                            vbR, lastCompleteDeletionArgs.getName());
                    break;
                }
            }
        }
        return rv;
    }

    /**
     * Take SystemEvent item and obtain the JSON manifest.
     * Next create a new/temp MockVBManifest from the JSON.
     * Finally check that this new object is equal to the test class's active
     *
     * @returns gtest assertion fail (with details) or success
     */
    ::testing::AssertionResult checkJson(const Item& manifest) {
        MockVBManifest newManifest(
                Collections::VB::Manifest::serialToJson(manifest));
        if (active != newManifest) {
            return ::testing::AssertionFailure() << "manifest mismatch\n"
                                                 << "generated\n"
                                                 << newManifest << "\nvs\n"
                                                 << active;
        }
        return ::testing::AssertionSuccess();
    }

    MockVBManifest active;
    MockVBManifest replica;
    EPStats global_stats;
    CheckpointConfig checkpoint_config;
    Configuration config;
    EPVBucket vbA;
    EPVBucket vbR;
    int64_t lastSeqno;
    Collections::Identifier lastCompleteDeletionArgs;

    static const int64_t snapEnd{200};
};

class VBucketManifestTest : public ::testing::Test {
public:
    ActiveReplicaManifest manifest;
};

TEST_F(VBucketManifestTest, collectionExists) {
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::","collections":[{"name":"vegetable","uid":"1"}]})"));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            {"vegetable::carrot", DocNamespace::Collections}));
    EXPECT_TRUE(manifest.isExclusiveOpen({"vegetable", 1}));
}

TEST_F(VBucketManifestTest, defaultCollectionExists) {
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            {"anykey", DocNamespace::DefaultCollection}));
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::","collections":[]})"));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            {"anykey", DocNamespace::DefaultCollection}));
}

TEST_F(VBucketManifestTest, add_delete_in_one_update) {
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::",)"
            R"("collections":[{"name":"$default","uid":"0"},)"
            R"(               {"name":"vegetable","uid":"1"}]})"));
    EXPECT_TRUE(manifest.isOpen({"vegetable", 1}));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            {"vegetable::cucumber", DocNamespace::Collections}));
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::",)"
            R"("collections":[{"name":"$default","uid":"0"},)"
            R"(               {"name":"vegetable","uid":"2"}]})"));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            {"vegetable::cucumber", DocNamespace::Collections}));
    EXPECT_TRUE(manifest.isOpen({"vegetable", 2}));
    EXPECT_TRUE(manifest.isDeleting({"vegetable", 2}));
}

TEST_F(VBucketManifestTest, updates) {
    EXPECT_TRUE(manifest.checkSize(1));
    EXPECT_TRUE(manifest.isExclusiveOpen({"$default", 0}));

    EXPECT_TRUE(manifest.update(
            R"({"separator":"::",)"
            R"("collections":[{"name":"$default","uid":"0"},)"
            R"(               {"name":"vegetable","uid":"1"}]})"));
    EXPECT_TRUE(manifest.checkSize(2));
    EXPECT_TRUE(manifest.isExclusiveOpen({"vegetable", 1}));

    EXPECT_TRUE(manifest.update(
            R"({"separator":"::",)"
            R"("collections":[{"name":"$default","uid":"0"},)"
            R"(               {"name":"vegetable","uid":"1"},)"
            R"(               {"name":"fruit","uid":"2"}]})"));
    EXPECT_TRUE(manifest.checkSize(3));
    EXPECT_TRUE(manifest.isExclusiveOpen({"fruit", 2}));

    EXPECT_TRUE(manifest.update(
            R"({"separator":"::",)"
            R"("collections":[{"name":"$default","uid":"0"},)"
            R"(               {"name":"vegetable","uid":"1"},)"
            R"(               {"name":"fruit","uid":"2"},)"
            R"(               {"name":"meat","uid":"3"},)"
            R"(               {"name":"dairy","uid":"4"}]})"));
    EXPECT_TRUE(manifest.checkSize(5));
    EXPECT_TRUE(manifest.isExclusiveOpen({"meat", 3}));
    EXPECT_TRUE(manifest.isExclusiveOpen({"dairy", 4}));
}

TEST_F(VBucketManifestTest, updates2) {
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::",)"
            R"("collections":[{"name":"$default","uid":"0"},)"
            R"(               {"name":"vegetable","uid":"1"},)"
            R"(               {"name":"fruit","uid":"2"},)"
            R"(               {"name":"meat","uid":"3"},)"
            R"(               {"name":"dairy","uid":"4"}]})"));
    EXPECT_TRUE(manifest.checkSize(5));

    // Remove meat and dairy, size is not affected because the delete is only
    // starting
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::",)"
            R"("collections":[{"name":"$default","uid":"0"},)"
            R"(               {"name":"vegetable","uid":"1"},)"
            R"(               {"name":"fruit","uid":"2"}]})"));
    EXPECT_TRUE(manifest.checkSize(5));
    EXPECT_TRUE(manifest.isExclusiveDeleting({"meat", 3}));
    EXPECT_TRUE(manifest.isExclusiveDeleting({"dairy", 4}));

    // But vegetable is accessible, the others are locked out
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            {"anykey", DocNamespace::DefaultCollection}));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            {"vegetable::carrot", DocNamespace::Collections}));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            {"dairy::milk", DocNamespace::Collections}));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            {"meat::chicken", DocNamespace::Collections}));
}

TEST_F(VBucketManifestTest, updates3) {
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::",)"
            R"("collections":[{"name":"$default","uid":"0"},)"
            R"(               {"name":"vegetable","uid":"1"},)"
            R"(               {"name":"fruit","uid":"2"},)"
            R"(               {"name":"meat","uid":"3"},)"
            R"(               {"name":"dairy","uid":"4"}]})"));
    EXPECT_TRUE(manifest.checkSize(5));

    // Remove everything
    EXPECT_TRUE(manifest.update(
            R"({ "separator":"::","collections":[]})"));
    EXPECT_TRUE(manifest.checkSize(5));
    EXPECT_TRUE(manifest.isExclusiveDeleting({"$default", 0}));
    EXPECT_TRUE(manifest.isExclusiveDeleting({"vegetable", 1}));
    EXPECT_TRUE(manifest.isExclusiveDeleting({"fruit", 2}));
    EXPECT_TRUE(manifest.isExclusiveDeleting({"meat", 3}));
    EXPECT_TRUE(manifest.isExclusiveDeleting({"dairy", 4}));

    // But vegetable is accessible, the others are 'locked' out
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            {"vegetable::carrot", DocNamespace::Collections}));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            {"dairy::milk", DocNamespace::Collections}));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            {"meat::chicken", DocNamespace::Collections}));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            {"fruit::apple", DocNamespace::Collections}));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            {"anykey", DocNamespace::DefaultCollection}));
}

TEST_F(VBucketManifestTest, add_beginDelete_add) {
    // add vegetable
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::","collections":[{"name":"vegetable","uid":"1"}]})"));
    auto seqno = manifest.getLastSeqno(); // seqno of the vegetable addition
    EXPECT_TRUE(manifest.checkSize(2));
    EXPECT_TRUE(manifest.isExclusiveOpen({"vegetable", 1}));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            {"vegetable::carrot", DocNamespace::Collections}));

    // The first manifest.update has dropped default collection and added
    // vegetable - test $default key with a seqno it could of existed with
    EXPECT_TRUE(manifest.isLogicallyDeleted(
            {"anykey", DocNamespace::DefaultCollection}, seqno - 1));
    // But vegetable is still good
    EXPECT_FALSE(manifest.isLogicallyDeleted(
            {"vegetable::carrot", DocNamespace::Collections}, seqno));

    // remove vegetable
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::","collections":[]})"));
    seqno = manifest.getLastSeqno();
    EXPECT_TRUE(manifest.checkSize(2));
    EXPECT_TRUE(manifest.isExclusiveDeleting({"vegetable", 1}));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            {"vegetable::carrot", DocNamespace::Collections}));

    // vegetable is now a deleting collection
    EXPECT_TRUE(manifest.isLogicallyDeleted(
            {"vegetable::carrot", DocNamespace::Collections}, seqno));

    // add vegetable a second time
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::","collections":[{"name":"vegetable","uid":"1"}]})"));
    auto oldSeqno = seqno;
    auto newSeqno = manifest.getLastSeqno();
    EXPECT_TRUE(manifest.checkSize(2));
    EXPECT_TRUE(manifest.isOpenAndDeleting({"vegetable", 1}));

    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            {"vegetable::carrot", DocNamespace::Collections}));

    // Now we expect older vegetables to be deleting and newer not to be.
    EXPECT_FALSE(manifest.isLogicallyDeleted(
            {"vegetable::carrot", DocNamespace::Collections}, newSeqno));
    EXPECT_TRUE(manifest.isLogicallyDeleted(
            {"vegetable::carrot", DocNamespace::Collections}, oldSeqno));
}

TEST_F(VBucketManifestTest, add_beginDelete_delete) {
    // add vegetable
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::","collections":[{"name":"vegetable","uid":"1"}]})"));
    EXPECT_TRUE(manifest.checkSize(2));
    EXPECT_TRUE(manifest.checkSize(2));
    EXPECT_TRUE(manifest.isExclusiveOpen({"vegetable", 1}));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            {"vegetable::carrot", DocNamespace::Collections}));

    // remove vegetable
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::","collections":[]})"));
    auto seqno = manifest.getLastSeqno();
    EXPECT_TRUE(manifest.checkSize(2));
    EXPECT_TRUE(manifest.isExclusiveDeleting({"vegetable", 1}));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            {"vegetable::carrot", DocNamespace::Collections}));
    EXPECT_TRUE(manifest.isLogicallyDeleted(
            {"vegetable::carrot", DocNamespace::Collections}, seqno));

    // finally remove vegetable
    EXPECT_TRUE(manifest.completeDeletion({"vegetable", 1}));
    EXPECT_TRUE(manifest.checkSize(1));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            {"vegetable::carrot", DocNamespace::Collections}));
}

TEST_F(VBucketManifestTest, add_beginDelete_add_delete) {
    // add vegetable:1
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::","collections":[{"name":"vegetable","uid":"1"}]})"));
    EXPECT_TRUE(manifest.checkSize(2));
    EXPECT_TRUE(manifest.isExclusiveOpen({"vegetable", 1}));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            {"vegetable::carrot", DocNamespace::Collections}));

    // remove vegetable
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::","collections":[]})"));
    EXPECT_TRUE(manifest.checkSize(2));
    EXPECT_TRUE(manifest.isExclusiveDeleting({"vegetable", 1}));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            {"vegetable::carrot", DocNamespace::Collections}));

    // add vegetable:2
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::","collections":[{"name":"vegetable","uid":"2"}]})"));
    EXPECT_TRUE(manifest.checkSize(2));
    EXPECT_TRUE(manifest.isOpenAndDeleting({"vegetable", 2}));

    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            {"vegetable::carrot", DocNamespace::Collections}));

    // finally remove vegetable:1
    EXPECT_TRUE(manifest.completeDeletion({"vegetable", 1}));
    EXPECT_TRUE(manifest.checkSize(2));

    // No longer OpenAndDeleting, now ExclusiveOpen
    EXPECT_TRUE(manifest.isExclusiveOpen({"vegetable", 2}));

    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            {"vegetable::carrot", DocNamespace::Collections}));
}

TEST_F(VBucketManifestTest, invalidDeletes) {
    // add vegetable
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::",)"
            R"("collections":[{"name":"$default","uid":"0"},)"
            R"(               {"name":"vegetable","uid":"1"}]})"));
    // Delete vegetable
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::",)"
            R"("collections":[{"name":"$default","uid":"0"}]})"));

    // Invalid.
    EXPECT_FALSE(manifest.completeDeletion({"unknown", 1}));
    EXPECT_FALSE(manifest.completeDeletion({"$default", 1}));

    EXPECT_TRUE(manifest.completeDeletion({"vegetable", 1}));

    // Delete $default
    EXPECT_TRUE(manifest.update(R"({"separator":"::",)"
                                R"("collections":[]})"));
    // Add $default
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::",)"
            R"("collections":[{"name":"$default","uid":"0"}]})"));
    EXPECT_TRUE(manifest.completeDeletion({"$default", 1}));
}

// Check that a deleting collection doesn't keep adding system events
TEST_F(VBucketManifestTest, doubleDelete) {
    auto seqno = manifest.getActiveVB().getHighSeqno();
    // add vegetable
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::",)"
            R"("collections":[{"name":"$default","uid":"0"},)"
            R"(               {"name":"vegetable","uid":"1"}]})"));
    EXPECT_LT(seqno, manifest.getActiveVB().getHighSeqno());
    seqno = manifest.getActiveVB().getHighSeqno();

    // Apply same manifest (different revision). Nothing will be created or
    // deleted. Apply direct to vbm, not via manifest.update as that would
    // complain about the lack of events
    manifest.getActiveManifest().wlock().update(
            manifest.getActiveVB(),
            {R"({"separator":"::",)"
             R"("collections":[{"name":"$default","uid":"0"},)"
             R"(               {"name":"vegetable","uid":"1"}]})"});

    EXPECT_EQ(seqno, manifest.getActiveVB().getHighSeqno());
    seqno = manifest.getActiveVB().getHighSeqno();

    // Now delete vegetable
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::","collections":[{"name":"$default","uid":"0"}]})"));

    EXPECT_LT(seqno, manifest.getActiveVB().getHighSeqno());
    seqno = manifest.getActiveVB().getHighSeqno();

    // same again, should have be nothing created or deleted
    manifest.getActiveManifest().wlock().update(
            manifest.getActiveVB(),
            {R"({"separator":"::",)"
             R"("collections":[{"name":"$default","uid":"0"}]})"});

    EXPECT_EQ(seqno, manifest.getActiveVB().getHighSeqno());
}

// This test changes the separator and propagates to the replica (all done
// via the noThrow helper functions).
TEST_F(VBucketManifestTest, active_replica_separatorChanges) {
    // Can change separator to @ as only default exists
    EXPECT_TRUE(manifest.update(
            R"({ "separator":"@", "collections":[{"name":"$default","uid":"0"}]})"));

    // Can change separator to / and add first collection
    EXPECT_TRUE(manifest.update(
            R"({ "separator":"/", "collections":[{"name":"$default","uid":"0"},)"
            R"(                                  {"name":"vegetable","uid":"1"}]})"));

    // Cannot change separator to ## because non-default collections exist
    EXPECT_FALSE(manifest.update(
            R"({ "separator":"##", "collections":[{"name":"$default","uid":"0"},)"
            R"(                                   {"name":"vegetable","uid":"1"}]})"));

    // Now just remove vegetable
    EXPECT_TRUE(manifest.update(
            R"({ "separator":"/", "collections":[{"name":"$default","uid":"0"}]})"));

    // vegetable still exists (isDeleting), but change to ##
    EXPECT_TRUE(manifest.update(
            R"({ "separator":"##", "collections":[{"name":"$default","uid":"0"}]})"));

    // Finish removal of vegetable
    EXPECT_TRUE(manifest.completeDeletion({"vegetable", 1}));

    // Can change separator as only default exists
    EXPECT_TRUE(manifest.update(
            R"({ "separator":"@", "collections":[{"name":"$default","uid":"0"}]})"));

    // Remove default
    EXPECT_TRUE(manifest.update(
            R"({ "separator":"/", "collections":[]})"));

    // $default still exists (isDeleting), so cannot change to ##
    EXPECT_TRUE(manifest.update(
            R"({ "separator":"##", "collections":[{"name":"$default","uid":"0"}]})"));

    EXPECT_TRUE(manifest.completeDeletion({"$default", 0}));

    // Can change separator as no collection exists
    EXPECT_TRUE(manifest.update(
            R"({ "separator":"-=-=-=-", "collections":[]})"));

    // Add a collection and check the new separator
    EXPECT_TRUE(manifest.update(
            R"({ "separator":"-=-=-=-", "collections":[{"name":"meat","uid":"3"}]})"));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            {"meat-=-=-=-bacon", DocNamespace::Collections}));
}

TEST_F(VBucketManifestTest, replica_add_remove) {
    // add vegetable
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::","collections":)"
            R"([{"name":"$default","uid":"0"},{"name":"vegetable","uid":"1"}]})"));

    // add meat & dairy
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::","collections":)"
            R"([{"name":"$default","uid":"0"},)"
            R"( {"name":"vegetable","uid":"1"},)"
            R"( {"name":"meat","uid":"3"},)"
            R"( {"name":"dairy","uid":"4"}]})"));

    // remove vegetable
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::","collections":)"
            R"([{"name":"$default","uid":"0"},)"
            R"( {"name":"meat","uid":"3"},)"
            R"( {"name":"dairy","uid":"4"}]})"));

    // remove $default
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::","collections":)"
            R"([{"name":"meat","uid":"3"},)"
            R"( {"name":"dairy","uid":"4"}]})"));

    // Check we can access the remaining collections
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            {"vegetable::carrot", DocNamespace::Collections}));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            {"anykey", DocNamespace::DefaultCollection}));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            {"meat::sausage", DocNamespace::Collections}));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            {"dairy::butter", DocNamespace::Collections}));
}

TEST_F(VBucketManifestTest, replica_add_remove_completeDelete) {
    // add vegetable
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::","collections":[{"name":"$default","uid":"0"},)"
            R"(                                 {"name":"vegetable","uid":"1"}]})"));

    // remove vegetable
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::","collections":[{"name":"$default","uid":"0"}]})"));

    // Finish removal of vegetable
    EXPECT_TRUE(manifest.completeDeletion({"vegetable", 1}));
}

class VBucketManifestTestEndSeqno : public VBucketManifestTest {};

TEST_F(VBucketManifestTestEndSeqno, singleAdd) {
    EXPECT_TRUE(
            manifest.checkGreatestEndSeqno(StoredValue::state_collection_open));
    EXPECT_TRUE(manifest.checkNumDeletingCollections(0));
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::","collections":[{"name":"$default","uid":"0"},)"
            R"(                                 {"name":"vegetable","uid":"1"}]})"));
    EXPECT_TRUE(
            manifest.checkGreatestEndSeqno(StoredValue::state_collection_open));
    EXPECT_TRUE(manifest.checkNumDeletingCollections(0));
    EXPECT_FALSE(manifest.isLogicallyDeleted(
            {"vegetable::sprout", DocNamespace::Collections}, 1));
}

TEST_F(VBucketManifestTestEndSeqno, singleDelete) {
    EXPECT_TRUE(
            manifest.checkGreatestEndSeqno(StoredValue::state_collection_open));
    EXPECT_TRUE(manifest.checkNumDeletingCollections(0));
    EXPECT_TRUE(manifest.update( // no collections left
            R"({"separator":"::","collections":[]})"));
    EXPECT_TRUE(manifest.checkGreatestEndSeqno(1));
    EXPECT_TRUE(manifest.checkNumDeletingCollections(1));
    EXPECT_TRUE(manifest.isLogicallyDeleted(
            {"vegetable::sprout", DocNamespace::DefaultCollection}, 1));
    EXPECT_FALSE(manifest.isLogicallyDeleted(
            {"vegetable::sprout", DocNamespace::DefaultCollection}, 2));
    EXPECT_TRUE(manifest.completeDeletion({"$default", 0}));
    EXPECT_TRUE(
            manifest.checkGreatestEndSeqno(StoredValue::state_collection_open));
    EXPECT_TRUE(manifest.checkNumDeletingCollections(0));
}

TEST_F(VBucketManifestTestEndSeqno, addDeleteAdd) {
    EXPECT_TRUE(
            manifest.checkGreatestEndSeqno(StoredValue::state_collection_open));
    EXPECT_TRUE(manifest.checkNumDeletingCollections(0));

    // Add
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::","collections":[{"name":"$default","uid":"0"},)"
            R"(                                 {"name":"vegetable","uid":"1"}]})"));

    // Delete
    EXPECT_TRUE(manifest.update(
            R"({"separator":"::","collections":[{"name":"$default","uid":"0"}]})"));

    EXPECT_TRUE(manifest.checkGreatestEndSeqno(2));
    EXPECT_TRUE(manifest.checkNumDeletingCollections(1));
    EXPECT_TRUE(manifest.isLogicallyDeleted(
            {"vegetable::sprout", DocNamespace::Collections}, 1));

    EXPECT_FALSE(manifest.isLogicallyDeleted(
            {"vegetable::sprout", DocNamespace::Collections}, 3));

    EXPECT_TRUE(manifest.update(
            R"({"separator":"::","collections":[{"name":"$default","uid":"0"},)"
            R"(                                 {"name":"vegetable","uid":"2"}]})"));

    EXPECT_TRUE(manifest.checkGreatestEndSeqno(2));
    EXPECT_TRUE(manifest.checkNumDeletingCollections(1));
    EXPECT_TRUE(manifest.isLogicallyDeleted(
            {"vegetable::sprout", DocNamespace::Collections}, 1));

    EXPECT_FALSE(manifest.isLogicallyDeleted(
            {"vegetable::sprout", DocNamespace::Collections}, 3));

    EXPECT_TRUE(manifest.completeDeletion({"vegetable", 1}));
    EXPECT_TRUE(
            manifest.checkGreatestEndSeqno(StoredValue::state_collection_open));
    EXPECT_TRUE(manifest.checkNumDeletingCollections(0));
}
