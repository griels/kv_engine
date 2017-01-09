/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc.
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
#include "daemon/mcbp.h"
#include "daemon/memcached.h"
#include "mutation_context.h"
#include <memcached/protocol_binary.h>
#include <memcached/types.h>
#include <include/memcached/types.h>

MutationCommandContext::MutationCommandContext(McbpConnection& c,
                                               protocol_binary_request_set* req,
                                               const ENGINE_STORE_OPERATION op_)
    : SteppableCommandContext(c),
      operation(req->message.header.request.cas == 0 ? op_ : OPERATION_CAS),
      key(req->bytes + sizeof(req->bytes),
          ntohs(req->message.header.request.keylen),
          DocNamespace::DefaultCollection),
      value(reinterpret_cast<const char*>(key.data() + key.size()),
            ntohl(req->message.header.request.bodylen) - key.size() -
            req->message.header.request.extlen),
      vbucket(ntohs(req->message.header.request.vbucket)),
      input_cas(ntohll(req->message.header.request.cas)),
      expiration(ntohl(req->message.body.expiration)),
      flags(req->message.body.flags),
      datatype(req->message.header.request.datatype),
      state(State::ValidateInput),
      newitem(nullptr, cb::ItemDeleter{c}) {
}
ENGINE_ERROR_CODE MutationCommandContext::step() {
    ENGINE_ERROR_CODE ret;
    do {
        switch (state) {
        case State::ValidateInput:
            ret = validateInput();
            break;
        case State::AllocateNewItem:
            ret = allocateNewItem();
            break;
        case State::StoreItem:
            ret = storeItem();
            break;
        case State::SendResponse:
            ret = sendResponse();
            break;
        case State::Done:
            if (operation == OPERATION_CAS) {
                SLAB_INCR(&connection, cas_hits);
            } else {
                SLAB_INCR(&connection, cmd_set);
            }
            return ENGINE_SUCCESS;
        }
    } while (ret == ENGINE_SUCCESS);

    if (ret != ENGINE_EWOULDBLOCK) {
        if (operation == OPERATION_CAS) {
            switch (ret) {
            case ENGINE_KEY_EEXISTS:
                SLAB_INCR(&connection, cas_badval);
                break;
            case ENGINE_KEY_ENOENT:
                get_thread_stats(&connection)->cas_misses++;
                break;
            default:;
            }
        } else {
            SLAB_INCR(&connection, cmd_set);
        }
    }

    return ret;
}

ENGINE_ERROR_CODE MutationCommandContext::validateInput() {
    if (!connection.isSupportsDatatype()) {
        if (datatype != PROTOCOL_BINARY_RAW_BYTES) {
            return ENGINE_EINVAL;
        }

        auto* validator = connection.getThread()->validator;
        try {
            auto* ptr = reinterpret_cast<const uint8_t*>(value.buf);
            if (validator->validate(ptr, value.len)) {
                datatype = PROTOCOL_BINARY_DATATYPE_JSON;
            }
        } catch (std::bad_alloc&) {
            return ENGINE_ENOMEM;
        }
    }

    state = State::AllocateNewItem;
    return ENGINE_SUCCESS;
}

ENGINE_ERROR_CODE MutationCommandContext::allocateNewItem() {
    item* it = nullptr;
    auto ret = bucket_allocate(&connection, &it, key, value.len,
                               flags, expiration, datatype, vbucket);

    if (ret != ENGINE_SUCCESS) {
        return ret;
    }
    newitem.reset(it);
    bucket_item_set_cas(&connection, newitem.get(), input_cas);
    item_info newitem_info;
    newitem_info.nvalue = 1;
    if (!bucket_get_item_info(&connection, newitem.get(), &newitem_info)) {
        return ENGINE_FAILED;
    }

    std::memcpy(newitem_info.value[0].iov_base, value.buf, value.len);
    state = State::StoreItem;

    return ENGINE_SUCCESS;
}

ENGINE_ERROR_CODE MutationCommandContext::storeItem() {
    uint64_t new_cas = input_cas;
    auto ret = bucket_store(&connection, newitem.get(), &new_cas, operation);
    if (ret == ENGINE_SUCCESS) {
        connection.setCAS(new_cas);
        state = State::SendResponse;
    } else if (ret == ENGINE_NOT_STORED) {
        // Need to remap error for add and replace
        if (operation == OPERATION_ADD) {
            ret = ENGINE_KEY_EEXISTS;
        } else if (operation == OPERATION_REPLACE) {
            ret = ENGINE_KEY_ENOENT;
        }
    }

    return ret;
}

ENGINE_ERROR_CODE MutationCommandContext::sendResponse() {
    update_topkeys(key, &connection);
    state = State::Done;

    if (connection.isNoReply()) {
        connection.setState(conn_new_cmd);
        return ENGINE_SUCCESS;
    }

    if (connection.isSupportsMutationExtras()) {
        item_info newitem_info;
        newitem_info.nvalue = 1;
        if (!bucket_get_item_info(&connection, newitem.get(), &newitem_info)) {
            return ENGINE_FAILED;
        }

        // Response includes vbucket UUID and sequence number
        // (in addition to value)
        mutation_descr_t extras;
        extras.vbucket_uuid = htonll(newitem_info.vbucket_uuid);
        extras.seqno = htonll(newitem_info.seqno);

        if (!mcbp_response_handler(nullptr, 0,
                                   &extras, sizeof(extras),
                                   nullptr, 0,
                                   PROTOCOL_BINARY_RAW_BYTES,
                                   PROTOCOL_BINARY_RESPONSE_SUCCESS,
                                   connection.getCAS(),
                                   connection.getCookie())) {
            return ENGINE_FAILED;
        }
        mcbp_write_and_free(&connection, &connection.getDynamicBuffer());
    } else {
        mcbp_write_packet(&connection, PROTOCOL_BINARY_RESPONSE_SUCCESS);
    }

    return ENGINE_SUCCESS;
}