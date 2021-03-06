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

#pragma once

#include <platform/sized_buffer.h>

namespace Collections {

// The reserved name of the system owned, default collection.
const char* const _DefaultCollectionIdentifier = "$default";
static cb::const_char_buffer DefaultCollectionIdentifier(
        _DefaultCollectionIdentifier);

// The default separator we will use for identifying collections in keys.
const char* const DefaultSeparator = "::";

// SystemEvent keys
const char* const SystemEventPrefix = "$collections";
const char* const DeleteEventKey = "delete";
const char* const SeparatorChangedKey = "separator";

// Couchstore private file name for manifest data
const char CouchstoreManifest[] = "_local/collections_manifest";

// Length of the string excluding the zero terminator (i.e. strlen)
const size_t CouchstoreManifestLen = sizeof(CouchstoreManifest) - 1;

using uid_t = uint64_t;

/**
 * Return a uid from a C-string.
 * A valid uid is a C-string where each character satisfies std::isxdigit
 * and can be converted to a uid_t by std::strtoull.
 *
 * @param uid C-string uid
 * @throws std::invalid_argument if uid is invalid
 */
uid_t makeUid(const char* uid);

/**
 * Interface definition for the a collection identifier - a pair of name and UID
 * Forces compile time dispatch for the 3 required methods.
 * getUid, getName and isDefaultCollection
 */
template <class T>
class IdentifierInterface {
public:
    /// @returns the UID for this identifier
    uid_t getUid() const {
        return static_cast<const T*>(this)->getUid();
    }

    /// @returns the name for this identifier
    cb::const_char_buffer getName() const {
        return static_cast<const T*>(this)->getName();
    }

    /// @returns true if the identifer's name matches the default name
    bool isDefaultCollection() const {
        return getName() == DefaultCollectionIdentifier;
    }
};

/**
 * A collection may exist concurrently, where one maybe open and the others
 * are in the process of being erased. This class carries the information for
 * locating the correct "generation" of a collection.
 */
class Identifier : public IdentifierInterface<Identifier> {
public:
    Identifier(cb::const_char_buffer name, uid_t uid) : name(name), uid(uid) {
    }

    template <class T>
    Identifier(const IdentifierInterface<T>& identifier)
        : name(identifier.getName()), uid(identifier.getUid()) {
    }

    cb::const_char_buffer getName() const {
        return name;
    }

    uid_t getUid() const {
        return uid;
    }

private:
    cb::const_char_buffer name;
    uid_t uid;
};

std::string to_string(const Identifier& identifier);

std::ostream& operator<<(std::ostream& os, const Identifier& identifier);

} // end namespace Collections
