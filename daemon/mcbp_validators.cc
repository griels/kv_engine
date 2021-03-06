/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc
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

#include "config.h"
#include "mcbp_validators.h"
#include <memcached/protocol_binary.h>
#include <include/memcached/protocol_binary.h>

#include "buckets.h"
#include "memcached.h"
#include "subdocument_validators.h"
#include "xattr/utils.h"

static inline bool may_accept_xattr(const Cookie& cookie) {
    auto* req = static_cast<protocol_binary_request_header*>(
            cookie.getPacketAsVoidPtr());
    if (mcbp::datatype::is_xattr(req->request.datatype)) {
        return cookie.getConnection().isXattrEnabled();
    }

    return true;
}

static inline bool may_accept_collections(const Cookie& cookie) {
    return cookie.getConnection().isDcpCollectionAware();
}

static inline std::string get_peer_description(const Cookie& cookie) {
    return cookie.getConnection().getDescription();
}

/******************************************************************************
 *                         Package validators                                 *
 *****************************************************************************/
static protocol_binary_response_status dcp_open_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_dcp_open*>(
            cookie.getPacketAsVoidPtr());

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 8 ||
        req->message.header.request.keylen == 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        /* INCORRECT FORMAT */
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // If there's a value, then OPEN_COLLECTIONS must be specified
    const uint32_t valuelen = ntohl(req->message.header.request.bodylen) -
                              req->message.header.request.extlen -
                              ntohs(req->message.header.request.keylen);

    const auto flags = ntohl(req->message.body.flags);
    if (!(flags & DCP_OPEN_COLLECTIONS) && valuelen) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // We could do these tests before checking the packet, but
    // it feels cleaner to validate the packet first.
    if (cookie.getConnection().getBucketEngine() == nullptr ||
        cookie.getConnection().getBucketEngine()->dcp.open == nullptr) {
        // The attached bucket does not support DCP
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    }

    const auto mask = DCP_OPEN_PRODUCER | DCP_OPEN_NOTIFIER |
                      DCP_OPEN_INCLUDE_XATTRS | DCP_OPEN_NO_VALUE |
                      DCP_OPEN_COLLECTIONS;

    if (flags & ~mask) {
        LOG_NOTICE(
                &cookie.getConnection(),
                "Client trying to open dcp stream with unknown flags (%08x) %s",
                flags,
                get_peer_description(cookie).c_str());
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    if ((flags & DCP_OPEN_NOTIFIER) && (flags & ~DCP_OPEN_NOTIFIER)) {
        LOG_NOTICE(&cookie.getConnection(),
                   "Invalid flags combination (%08x) specified for a DCP "
                   "consumer %s",
                   flags,
                   get_peer_description(cookie).c_str());
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status dcp_add_stream_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_dcp_add_stream*>(
            cookie.getPacketAsVoidPtr());
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 4 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != htonl(4) ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        /* INCORRECT FORMAT */
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // We could do these tests before checking the packet, but
    // it feels cleaner to validate the packet first.
    if (cookie.getConnection().getBucketEngine() == nullptr ||
        cookie.getConnection().getBucketEngine()->dcp.add_stream == nullptr) {
        // The attached bucket does not support DCP
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    }

    const auto flags = ntohl(req->message.body.flags);
    const auto mask = DCP_ADD_STREAM_FLAG_TAKEOVER |
                      DCP_ADD_STREAM_FLAG_DISKONLY |
                      DCP_ADD_STREAM_FLAG_LATEST |
                      DCP_ADD_STREAM_ACTIVE_VB_ONLY;

    if (flags & ~mask) {
        if (flags & DCP_ADD_STREAM_FLAG_NO_VALUE) {
            // MB-22525 The NO_VALUE flag should be passed to DCP_OPEN
            LOG_NOTICE(&cookie.getConnection(),
                       "Client trying to add stream with NO VALUE %s",
                       get_peer_description(cookie).c_str());
        } else {
            LOG_NOTICE(
                    &cookie.getConnection(),
                    "Client trying to add stream with unknown flags (%08x) %s",
                    flags,
                    get_peer_description(cookie).c_str());
        }
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status dcp_close_stream_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_dcp_close_stream*>(
            cookie.getPacketAsVoidPtr());
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        /* INCORRECT FORMAT */
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // We could do these tests before checking the packet, but
    // it feels cleaner to validate the packet first.
    if (cookie.getConnection().getBucketEngine() == nullptr ||
        cookie.getConnection().getBucketEngine()->dcp.close_stream == nullptr) {
        // The attached bucket does not support DCP
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status dcp_get_failover_log_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_dcp_get_failover_log*>(
            cookie.getPacketAsVoidPtr());
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // We could do these tests before checking the packet, but
    // it feels cleaner to validate the packet first.
    if (cookie.getConnection().getBucketEngine() == nullptr ||
        cookie.getConnection().getBucketEngine()->dcp.get_failover_log ==
                nullptr) {
        // The attached bucket does not support DCP
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status dcp_stream_req_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_dcp_stream_req*>(
            cookie.getPacketAsVoidPtr());
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 5*sizeof(uint64_t) + 2*sizeof(uint32_t) ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        /* INCORRECT FORMAT */
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // We could do these tests before checking the packet, but
    // it feels cleaner to validate the packet first.
    if (cookie.getConnection().getBucketEngine() == nullptr ||
        cookie.getConnection().getBucketEngine()->dcp.stream_req == nullptr) {
        // The attached bucket does not support DCP
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status dcp_stream_end_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_dcp_stream_end*>(
            cookie.getPacketAsVoidPtr());
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 4 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != htonl(4) ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // We could do these tests before checking the packet, but
    // it feels cleaner to validate the packet first.
    if (cookie.getConnection().getBucketEngine() == nullptr ||
        cookie.getConnection().getBucketEngine()->dcp.stream_end == nullptr) {
        // The attached bucket does not support DCP
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status dcp_snapshot_marker_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_dcp_snapshot_marker*>(
            cookie.getPacketAsVoidPtr());
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 20 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != htonl(20) ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // We could do these tests before checking the packet, but
    // it feels cleaner to validate the packet first.
    if (cookie.getConnection().getBucketEngine() == nullptr ||
        cookie.getConnection().getBucketEngine()->dcp.snapshot_marker ==
                nullptr) {
        // The attached bucket does not support DCP
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status dcp_system_event_validator(
        const Cookie& cookie) {
    // keylen + bodylen > ??
    auto req = static_cast<protocol_binary_request_dcp_system_event*>(
            cookie.getPacketAsVoidPtr());
    auto bodylen = ntohl(req->message.header.request.bodylen);
    auto keylen = ntohs(req->message.header.request.keylen);
    auto extlen = req->message.header.request.extlen;

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        extlen != protocol_binary_request_dcp_system_event::getExtrasLength() ||
        (extlen + keylen) > bodylen) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    if (!mcbp::systemevent::validate(ntohl(req->message.body.event))) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // We could do these tests before checking the packet, but
    // it feels cleaner to validate the packet first.
    if (cookie.getConnection().getBucketEngine() == nullptr ||
        cookie.getConnection().getBucketEngine()->dcp.system_event == nullptr) {
        // The attached bucket does not support DCP
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static bool is_valid_xattr_blob(const protocol_binary_request_header& header) {
    const uint32_t extlen{header.request.extlen};
    const uint32_t keylen{ntohs(header.request.keylen)};
    const uint32_t bodylen{ntohl(header.request.bodylen)};

    auto* ptr = reinterpret_cast<const char*>(header.bytes);
    ptr += sizeof(header.bytes) + extlen + keylen;

    cb::const_char_buffer xattr{ptr, bodylen - keylen - extlen};
    return cb::xattr::validate(xattr);
}

static protocol_binary_response_status dcp_mutation_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_dcp_mutation*>(
            cookie.getPacketAsVoidPtr());
    const auto datatype = req->message.header.request.datatype;

    const uint32_t extlen{req->message.header.request.extlen};
    const uint32_t keylen{ntohs(req->message.header.request.keylen)};
    const uint32_t bodylen{ntohl(req->message.header.request.bodylen)};

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        keylen == 0 || bodylen == 0 || (keylen + extlen) > bodylen ||
        !mcbp::datatype::is_valid(datatype) || !may_accept_xattr(cookie)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // extlen varies for collection aware DCP vs legacy
    if (extlen != protocol_binary_request_dcp_mutation::getExtrasLength(
                          may_accept_collections(cookie))) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    if (mcbp::datatype::is_xattr(datatype) &&
        !is_valid_xattr_blob(req->message.header)) {
        return PROTOCOL_BINARY_RESPONSE_XATTR_EINVAL;
    }

    // We could do these tests before checking the packet, but
    // it feels cleaner to validate the packet first.
    if (cookie.getConnection().getBucketEngine() == nullptr ||
        cookie.getConnection().getBucketEngine()->dcp.mutation == nullptr) {
        // The attached bucket does not support DCP
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status dcp_deletion_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_dcp_deletion*>(
            cookie.getPacketAsVoidPtr());
    const auto datatype = req->message.header.request.datatype;

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.keylen == 0) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // Check datatype - only allow raw, or XATTR iff XATTRs are enabled.
    if (!(mcbp::datatype::is_raw(datatype) ||
          (datatype == PROTOCOL_BINARY_DATATYPE_XATTR &&
           may_accept_xattr(cookie)))) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    const uint32_t extlen{req->message.header.request.extlen};
    // extlen varies for collection aware DCP vs legacy
    if (extlen != protocol_binary_request_dcp_deletion::getExtrasLength(
                          may_accept_collections(cookie))) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // We could do these tests before checking the packet, but
    // it feels cleaner to validate the packet first.
    if (cookie.getConnection().getBucketEngine() == nullptr ||
        cookie.getConnection().getBucketEngine()->dcp.deletion == nullptr) {
        // The attached bucket does not support DCP
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status dcp_expiration_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_dcp_deletion*>(
            cookie.getPacketAsVoidPtr());
    uint16_t klen = ntohs(req->message.header.request.keylen);
    uint32_t bodylen = ntohl(req->message.header.request.bodylen) - klen;
    bodylen -= req->message.header.request.extlen;
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.keylen == 0 || bodylen != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    const uint32_t extlen{req->message.header.request.extlen};
    // extlen varies for collection aware DCP vs legacy
    if (extlen != protocol_binary_request_dcp_deletion::getExtrasLength(
                          may_accept_collections(cookie))) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // We could do these tests before checking the packet, but
    // it feels cleaner to validate the packet first.
    if (cookie.getConnection().getBucketEngine() == nullptr ||
        cookie.getConnection().getBucketEngine()->dcp.expiration == nullptr) {
        // The attached bucket does not support DCP
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status dcp_flush_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_dcp_flush*>(
            cookie.getPacketAsVoidPtr());
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // We could do these tests before checking the packet, but
    // it feels cleaner to validate the packet first.
    if (cookie.getConnection().getBucketEngine() == nullptr ||
        cookie.getConnection().getBucketEngine()->dcp.flush == nullptr) {
        // The attached bucket does not support DCP
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status dcp_set_vbucket_state_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_dcp_set_vbucket_state*>(
            cookie.getPacketAsVoidPtr());
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 1 ||
        req->message.header.request.keylen != 0 ||
        ntohl(req->message.header.request.bodylen) != 1 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    if (req->message.body.state < 1 || req->message.body.state > 4) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // We could do these tests before checking the packet, but
    // it feels cleaner to validate the packet first.
    if (cookie.getConnection().getBucketEngine() == nullptr ||
        cookie.getConnection().getBucketEngine()->dcp.set_vbucket_state ==
                nullptr) {
        // The attached bucket does not support DCP
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status dcp_noop_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_dcp_noop*>(
            cookie.getPacketAsVoidPtr());
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // We could do these tests before checking the packet, but
    // it feels cleaner to validate the packet first.
    if (cookie.getConnection().getBucketEngine() == nullptr ||
        cookie.getConnection().getBucketEngine()->dcp.noop == nullptr) {
        // The attached bucket does not support DCP
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status dcp_buffer_acknowledgement_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_dcp_buffer_acknowledgement*>(
            cookie.getPacketAsVoidPtr());
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 4 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != ntohl(4) ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // We could do these tests before checking the packet, but
    // it feels cleaner to validate the packet first.
    if (cookie.getConnection().getBucketEngine() == nullptr ||
        cookie.getConnection().getBucketEngine()->dcp.buffer_acknowledgement ==
                nullptr) {
        // The attached bucket does not support DCP
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status dcp_control_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_dcp_control*>(
            cookie.getPacketAsVoidPtr());
    uint16_t nkey = ntohs(req->message.header.request.keylen);
    uint32_t nval = ntohl(req->message.header.request.bodylen) - nkey;

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 || nkey == 0 || nval == 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // We could do these tests before checking the packet, but
    // it feels cleaner to validate the packet first.
    if (cookie.getConnection().getBucketEngine() == nullptr ||
        cookie.getConnection().getBucketEngine()->dcp.control == nullptr) {
        // The attached bucket does not support DCP
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status configuration_refresh_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.cas != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status verbosity_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 4 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.cas != 0 ||
        ntohl(req->message.header.request.bodylen) != 4 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status hello_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());
    uint32_t len = ntohl(req->message.header.request.bodylen);
    len -= ntohs(req->message.header.request.keylen);

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 || (len % 2) != 0 ||
        req->message.header.request.cas != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status version_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.cas != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status quit_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES ||
        req->message.header.request.cas != 0) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status sasl_list_mech_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.cas != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status sasl_auth_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen == 0 ||
        req->message.header.request.cas != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status noop_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES ||
        req->message.header.request.cas != 0) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status flush_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());
    uint8_t extlen = req->message.header.request.extlen;
    uint32_t bodylen = ntohl(req->message.header.request.bodylen);

    if (extlen != 0 && extlen != 4) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    if (bodylen != extlen) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    if (extlen == 4) {
        // We don't support delayed flush anymore
        auto* req = reinterpret_cast<protocol_binary_request_flush*>(
                cookie.getPacketAsVoidPtr());
        if (req->message.body.expiration != 0) {
            return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
        }
    }

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES ||
        req->message.header.request.cas != 0) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status add_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());
    /* Must have extras and key, may have value */
    auto datatype = req->message.header.request.datatype;

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 8 ||
        req->message.header.request.keylen == 0 ||
        req->message.header.request.cas != 0 ||
        mcbp::datatype::is_xattr(datatype) ||
        !mcbp::datatype::is_valid(datatype)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status set_replace_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());
    /* Must have extras and key, may have value */
    auto datatype = req->message.header.request.datatype;

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 8 ||
        req->message.header.request.keylen == 0 ||
        mcbp::datatype::is_xattr(datatype) ||
        !mcbp::datatype::is_valid(datatype)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status append_prepend_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());
    /* Must not have extras, must have key, may have value */
    auto datatype = req->message.header.request.datatype;

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen == 0 ||
        mcbp::datatype::is_xattr(datatype) ||
        !mcbp::datatype::is_valid(datatype)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status get_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());
    uint16_t klen = ntohs(req->message.header.request.keylen);
    uint32_t blen = ntohl(req->message.header.request.bodylen);

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        klen == 0 || klen != blen ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES ||
        req->message.header.request.cas != 0) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status gat_validator(const Cookie& cookie) {
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());
    uint32_t klen = ntohs(req->message.header.request.keylen);
    uint32_t blen = ntohl(req->message.header.request.bodylen);

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 4 || klen == 0 ||
        (klen + 4) != blen ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES ||
        req->message.header.request.cas != 0) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status delete_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());
    uint16_t klen = ntohs(req->message.header.request.keylen);
    uint32_t blen = ntohl(req->message.header.request.bodylen);

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        klen == 0 || klen != blen ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status stat_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());
    uint16_t klen = ntohs(req->message.header.request.keylen);
    uint32_t blen = ntohl(req->message.header.request.bodylen);

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 || klen != blen ||
        req->message.header.request.cas != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status arithmetic_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());
    uint16_t klen = ntohs(req->message.header.request.keylen);
    uint32_t blen = ntohl(req->message.header.request.bodylen);
    uint8_t extlen = req->message.header.request.extlen;

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        extlen != 20 || klen == 0 || (klen + extlen) != blen ||
        req->message.header.request.cas != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status get_cmd_timer_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());
    uint16_t klen = ntohs(req->message.header.request.keylen);
    uint32_t blen = ntohl(req->message.header.request.bodylen);
    uint8_t extlen = req->message.header.request.extlen;

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        extlen != 1 || (klen + extlen) != blen ||
        req->message.header.request.cas != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status set_ctrl_token_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_set_ctrl_token*>(
            cookie.getPacketAsVoidPtr());

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != sizeof(uint64_t) ||
        req->message.header.request.keylen != 0 ||
        ntohl(req->message.header.request.bodylen) != sizeof(uint64_t) ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES ||
        req->message.body.new_cas == 0) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status get_ctrl_token_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.cas != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status ioctl_get_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_ioctl_get*>(
            cookie.getPacketAsVoidPtr());
    uint16_t klen = ntohs(req->message.header.request.keylen);
    uint32_t blen = ntohl(req->message.header.request.bodylen);

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        klen == 0 || klen != blen || klen > IOCTL_KEY_LENGTH ||
        req->message.header.request.cas != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status ioctl_set_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_ioctl_set*>(
            cookie.getPacketAsVoidPtr());
    uint16_t klen = ntohs(req->message.header.request.keylen);
    size_t vallen = ntohl(req->message.header.request.bodylen) - klen;

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.cas != 0 ||
        klen == 0 || klen > IOCTL_KEY_LENGTH ||
        vallen > IOCTL_VAL_LENGTH ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status audit_put_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_audit_put*>(
            cookie.getPacketAsVoidPtr());

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 4 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.cas != 0 ||
        ntohl(req->message.header.request.bodylen) <= 4 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status audit_config_reload_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.cas != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status observe_seqno_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());
    uint32_t bodylen = ntohl(req->message.header.request.bodylen);

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        bodylen != 8 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status get_adjusted_time_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_get_adjusted_time*>(
            cookie.getPacketAsVoidPtr());

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.cas != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status set_drift_counter_state_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_set_drift_counter_state*>(
            cookie.getPacketAsVoidPtr());

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != sizeof(uint8_t) + sizeof(int64_t) ||
        req->message.header.request.keylen != 0 ||
        ntohl(req->message.header.request.bodylen) != sizeof(uint8_t) + sizeof(int64_t) ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

/**
 * The create bucket contains message have the following format:
 *    key: bucket name
 *    body: module\nconfig
 */
static protocol_binary_response_status create_bucket_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());

    uint16_t klen = ntohs(req->message.header.request.keylen);
    uint32_t blen = ntohl(req->message.header.request.bodylen);
    uint8_t extlen = req->message.header.request.extlen;

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        extlen != 0 || klen == 0 || klen > MAX_BUCKET_NAME_LENGTH ||
        /* The packet needs a body with the information of the bucket
         * to create
         */
        (blen - klen) == 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status list_bucket_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status delete_bucket_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.keylen == 0 ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.bodylen == 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status select_bucket_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());

    uint16_t klen = ntohs(req->message.header.request.keylen);
    uint32_t blen = ntohl(req->message.header.request.bodylen);

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        klen != blen || req->message.header.request.extlen != 0 ||
        klen > 1023 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status get_all_vb_seqnos_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_get_all_vb_seqnos*>(
            cookie.getPacketAsVoidPtr());
    uint16_t klen = ntohs(req->message.header.request.keylen);
    uint32_t blen = ntohl(req->message.header.request.bodylen);
    uint8_t extlen = req->message.header.request.extlen;

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        klen != 0 || extlen != blen ||
        req->message.header.request.cas != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    if (extlen != 0) {
        // extlen is optional, and if non-zero it contains the vbucket
        // state to report
        if (extlen != sizeof(vbucket_state_t)) {
            return PROTOCOL_BINARY_RESPONSE_EINVAL;
        }
        vbucket_state_t state;
        memcpy(&state, &req->message.body.state, sizeof(vbucket_state_t));
        state = static_cast<vbucket_state_t>(ntohl(state));
        if (!is_valid_vbucket_state_t(state)) {
            return PROTOCOL_BINARY_RESPONSE_EINVAL;
        }
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status shutdown_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.cas == 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}



static protocol_binary_response_status get_meta_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());
    uint32_t extlen = req->message.header.request.extlen;
    uint32_t klen = ntohs(req->message.header.request.keylen);
    uint32_t blen = ntohl(req->message.header.request.bodylen);

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        extlen > 1 || klen == 0 || (klen + extlen) != blen ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES ||
        req->message.header.request.cas != 0) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    if (extlen == 1) {
        const uint8_t* extdata = req->bytes + sizeof(req->bytes);
        if (*extdata > 2) {
            // 1 == return conflict resolution mode
            // 2 == return datatype
            return PROTOCOL_BINARY_RESPONSE_EINVAL;
        }
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status mutate_with_meta_validator(const Cookie& cookie) {
    auto req = static_cast<protocol_binary_request_get_meta*>(
            cookie.getPacketAsVoidPtr());

    const uint32_t extlen = req->message.header.request.extlen;
    const uint32_t keylen = ntohs(req->message.header.request.keylen);
    const uint32_t bodylen = ntohl(req->message.header.request.bodylen);
    const auto datatype = req->message.header.request.datatype;

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        keylen == 0 || (keylen + extlen) > bodylen ||
        !may_accept_xattr(cookie) ||
        !mcbp::datatype::is_valid(datatype)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    if (mcbp::datatype::is_xattr(datatype) && !may_accept_xattr(cookie)) {
        // Datatype is XATTR and xattrs is not supported in the connection.
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // revid_nbytes, flags and exptime is mandatory fields.. and we need a key
    // extlen, the size dicates what is encoded.
    switch (extlen) {
    case 24: // no nmeta and no options
    case 26: // nmeta
    case 28: // options (4-byte field)
    case 30: // options and nmeta (options followed by nmeta)
        break;
    default:
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    if (mcbp::datatype::is_xattr(datatype) &&
        !is_valid_xattr_blob(req->message.header)) {
        return PROTOCOL_BINARY_RESPONSE_XATTR_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status get_errmap_validator(const Cookie& cookie) {
    const auto& hdr = *static_cast<const protocol_binary_request_header*>(
            cookie.getPacketAsVoidPtr());
    if (hdr.request.magic == PROTOCOL_BINARY_REQ &&
            ntohl(hdr.request.bodylen) == 2 &&
            hdr.request.cas == 0 &&
            hdr.request.keylen == 0 &&
            hdr.request.vbucket == 0 &&
            hdr.request.extlen == 0 &&
            hdr.request.datatype == PROTOCOL_BINARY_RAW_BYTES) {
        return PROTOCOL_BINARY_RESPONSE_SUCCESS;
    } else {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
}

static protocol_binary_response_status get_locked_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());
    uint32_t extlen = req->message.header.request.extlen;
    uint32_t klen = ntohs(req->message.header.request.keylen);
    uint32_t blen = ntohl(req->message.header.request.bodylen);

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        klen == 0 || (klen + extlen) != blen || (extlen != 0 && extlen != 4) ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES ||
        req->message.header.request.cas != 0) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status unlock_validator(const Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());
    uint32_t extlen = req->message.header.request.extlen;
    uint32_t klen = ntohs(req->message.header.request.keylen);
    uint32_t blen = ntohl(req->message.header.request.bodylen);

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        klen == 0 || extlen != 0 || klen != blen ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES ||
        req->message.header.request.cas == 0) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status collections_set_manifest_validator(
        const Cookie& cookie) {
    auto packet = static_cast<protocol_binary_collections_set_manifest*>(
            cookie.getPacketAsVoidPtr());
    auto& req = packet->message.header.request;

    if (req.magic != PROTOCOL_BINARY_REQ || req.keylen != 0 ||
        req.extlen != 0 || req.cas != 0 || req.datatype != 0 ||
        req.vbucket != 0 || req.bodylen == 0) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // We could do these tests before checking the packet, but
    // it feels cleaner to validate the packet first.
    if (cookie.getConnection().getBucketEngine() == nullptr ||
        cookie.getConnection().getBucketEngine()->collections.set_manifest ==
                nullptr) {
        // The attached bucket does not support collections
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

void McbpValidatorChains::initializeMcbpValidatorChains(McbpValidatorChains& chains) {
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_OPEN, dcp_open_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_ADD_STREAM, dcp_add_stream_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_CLOSE_STREAM, dcp_close_stream_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_SNAPSHOT_MARKER, dcp_snapshot_marker_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_DELETION, dcp_deletion_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_EXPIRATION, dcp_expiration_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_FLUSH, dcp_flush_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_GET_FAILOVER_LOG, dcp_get_failover_log_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_MUTATION, dcp_mutation_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_SET_VBUCKET_STATE, dcp_set_vbucket_state_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_NOOP, dcp_noop_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_BUFFER_ACKNOWLEDGEMENT, dcp_buffer_acknowledgement_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_CONTROL, dcp_control_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_STREAM_END, dcp_stream_end_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_STREAM_REQ, dcp_stream_req_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_SYSTEM_EVENT, dcp_system_event_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_ISASL_REFRESH, configuration_refresh_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SSL_CERTS_REFRESH, configuration_refresh_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_VERBOSITY, verbosity_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_HELLO, hello_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_VERSION, version_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_QUIT, quit_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_QUITQ, quit_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SASL_LIST_MECHS, sasl_list_mech_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SASL_AUTH, sasl_auth_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SASL_STEP, sasl_auth_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_NOOP, noop_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_FLUSH, flush_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_FLUSHQ, flush_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GET, get_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GETQ, get_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GETK, get_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GETKQ, get_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GAT, gat_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GATQ, gat_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_TOUCH, gat_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DELETE, delete_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DELETEQ, delete_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_STAT, stat_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_INCREMENT, arithmetic_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_INCREMENTQ, arithmetic_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DECREMENT, arithmetic_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DECREMENTQ, arithmetic_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GET_CMD_TIMER, get_cmd_timer_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SET_CTRL_TOKEN, set_ctrl_token_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GET_CTRL_TOKEN, get_ctrl_token_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_IOCTL_GET, ioctl_get_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_IOCTL_SET, ioctl_set_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_AUDIT_PUT, audit_put_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_AUDIT_CONFIG_RELOAD, audit_config_reload_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SHUTDOWN, shutdown_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_OBSERVE_SEQNO, observe_seqno_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GET_ADJUSTED_TIME, get_adjusted_time_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SET_DRIFT_COUNTER_STATE, set_drift_counter_state_validator);

    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_GET, subdoc_get_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_EXISTS, subdoc_exists_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_DICT_ADD, subdoc_dict_add_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_DICT_UPSERT, subdoc_dict_upsert_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_DELETE, subdoc_delete_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_REPLACE, subdoc_replace_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_PUSH_LAST, subdoc_array_push_last_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_PUSH_FIRST, subdoc_array_push_first_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_INSERT, subdoc_array_insert_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_ADD_UNIQUE, subdoc_array_add_unique_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_COUNTER, subdoc_counter_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_MULTI_LOOKUP, subdoc_multi_lookup_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_MULTI_MUTATION, subdoc_multi_mutation_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_GET_COUNT, subdoc_get_count_validator);

    chains.push_unique(PROTOCOL_BINARY_CMD_SETQ, set_replace_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SET, set_replace_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_ADDQ, add_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_ADD, add_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_REPLACEQ, set_replace_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_REPLACE, set_replace_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_APPENDQ, append_prepend_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_APPEND, append_prepend_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_PREPENDQ, append_prepend_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_PREPEND, append_prepend_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_CREATE_BUCKET, create_bucket_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_LIST_BUCKETS, list_bucket_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DELETE_BUCKET, delete_bucket_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SELECT_BUCKET, select_bucket_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GET_ALL_VB_SEQNOS, get_all_vb_seqnos_validator);

    chains.push_unique(PROTOCOL_BINARY_CMD_GET_META, get_meta_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GETQ_META, get_meta_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SET_WITH_META, mutate_with_meta_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SETQ_WITH_META, mutate_with_meta_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_ADD_WITH_META, mutate_with_meta_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_ADDQ_WITH_META, mutate_with_meta_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DEL_WITH_META, mutate_with_meta_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DELQ_WITH_META, mutate_with_meta_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GET_ERROR_MAP, get_errmap_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GET_LOCKED, get_locked_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_UNLOCK_KEY, unlock_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_RBAC_REFRESH, configuration_refresh_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_COLLECTIONS_SET_MANIFEST,
                       collections_set_manifest_validator);
}
