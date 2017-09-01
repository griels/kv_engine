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

#include "kvstore_config.h"

/// A listener class to update KVStore related configs at runtime.
class KVStoreConfig::ConfigChangeListener : public ValueChangedListener {
public:
    ConfigChangeListener(KVStoreConfig& c) : config(c) {
    }

    void sizeValueChanged(const std::string& key, size_t value) override {
        if (key == "fsync_after_every_n_bytes_written") {
            config.setPeriodicSyncBytes(value);
        }
    }

private:
    KVStoreConfig& config;
};

KVStoreConfig::KVStoreConfig(Configuration& config, uint16_t shardid)
    : KVStoreConfig(config.getMaxVbuckets(),
                    config.getMaxNumShards(),
                    config.getDbname(),
                    config.getBackend(),
                    shardid,
                    config.isCollectionsPrototypeEnabled(),
                    config.getRocksdbWriteBufferSize(),
                    config.getRocksdbDbWriteBufferSize(),
                    config.getRocksdbMaxWriteBufferNumber()) {
    setPeriodicSyncBytes(config.getFsyncAfterEveryNBytesWritten());
    config.addValueChangedListener("fsync_after_every_n_bytes_written",
                                   new ConfigChangeListener(*this));
}

KVStoreConfig::KVStoreConfig(uint16_t _maxVBuckets,
                             uint16_t _maxShards,
                             const std::string& _dbname,
                             const std::string& _backend,
                             uint16_t _shardId,
                             bool _persistDocNamespace,
                             size_t writeBufferSize,
                             size_t dbWriteBufferSize,
                             size_t maxWriteBufferNumber)
    : maxVBuckets(_maxVBuckets),
      maxShards(_maxShards),
      dbname(_dbname),
      backend(_backend),
      shardId(_shardId),
      logger(&global_logger),
      buffered(true),
      persistDocNamespace(_persistDocNamespace),
      writeBufferSize(writeBufferSize),
      dbWriteBufferSize(dbWriteBufferSize),
      maxWriteBufferNumber(maxWriteBufferNumber) {
}

KVStoreConfig& KVStoreConfig::setLogger(Logger& _logger) {
    logger = &_logger;
    return *this;
}

KVStoreConfig& KVStoreConfig::setBuffered(bool _buffered) {
    buffered = _buffered;
    return *this;
}