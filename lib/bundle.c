/* Copyright (c) 2011, 2012, 2013, 2014, 2015, 2016, 2017 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include "bundle.h"

#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <inttypes.h>

#include "colors.h"
#include "multipath.h"
#include "nx-match.h"
#include "openflow/nicira-ext.h"
#include "openvswitch/dynamic-string.h"
#include "openvswitch/meta-flow.h"
#include "openvswitch/ofp-actions.h"
#include "openvswitch/ofp-errors.h"
#include "openvswitch/ofp-port.h"
#include "openvswitch/ofpbuf.h"
#include "openvswitch/vlog.h"
#include "util.h"

VLOG_DEFINE_THIS_MODULE(bundle);

static ofp_port_t
execute_ab(const struct ofpact_bundle *bundle,
           bool (*slave_enabled)(ofp_port_t ofp_port, void *aux), void *aux)
{
    size_t i;

    for (i = 0; i < bundle->n_slaves; i++) {
        ofp_port_t slave = bundle->slaves[i];
        if (slave_enabled(slave, aux)) {
            return slave;
        }
    }

    return OFPP_NONE;
}

static ofp_port_t
execute_hrw(const struct ofpact_bundle *bundle,
            const struct flow *flow, struct flow_wildcards *wc,
            bool (*slave_enabled)(ofp_port_t ofp_port, void *aux), void *aux)
{
    uint32_t flow_hash, best_hash;
    int best, i;

    if (bundle->n_slaves > 1) {
        flow_mask_hash_fields(flow, wc, bundle->fields);
    }

    flow_hash = flow_hash_fields(flow, bundle->fields, bundle->basis);
    best = -1;
    best_hash = 0;

    for (i = 0; i < bundle->n_slaves; i++) {
        if (slave_enabled(bundle->slaves[i], aux)) {
            uint32_t hash = hash_2words(i, flow_hash);

            if (best < 0 || hash > best_hash) {
                best_hash = hash;
                best = i;
            }
        }
    }

    return best >= 0 ? bundle->slaves[best] : OFPP_NONE;
}

/* Executes 'bundle' on 'flow'.  Sets fields in 'wc' that were used to
 * calculate the result.  Uses 'slave_enabled' to determine if the slave
 * designated by 'ofp_port' is up.  Returns the chosen slave, or
 * OFPP_NONE if none of the slaves are acceptable. */
ofp_port_t
bundle_execute(const struct ofpact_bundle *bundle,
               const struct flow *flow, struct flow_wildcards *wc,
               bool (*slave_enabled)(ofp_port_t ofp_port, void *aux),
               void *aux)
{
    switch (bundle->algorithm) {
    case NX_BD_ALG_HRW:
        return execute_hrw(bundle, flow, wc, slave_enabled, aux);

    case NX_BD_ALG_ACTIVE_BACKUP:
        return execute_ab(bundle, slave_enabled, aux);

    default:
        OVS_NOT_REACHED();
    }
}

enum ofperr
bundle_check(const struct ofpact_bundle *bundle, ofp_port_t max_ports,
             const struct match *match)
{
    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
    size_t i;

    if (bundle->dst.field) {
        enum ofperr error = mf_check_dst(&bundle->dst, match);
        if (error) {
            return error;
        }
    }

    for (i = 0; i < bundle->n_slaves; i++) {
        ofp_port_t ofp_port = bundle->slaves[i];

        if (ofp_port != OFPP_NONE) {
            enum ofperr error = ofpact_check_output_port(ofp_port, max_ports);
            if (error) {
                VLOG_WARN_RL(&rl, "invalid slave %"PRIu32, ofp_port);
                return error;
            }
        }
        /* Controller slaves are unsupported due to the lack of a max_len
         * argument. This may or may not change in the future.  There doesn't
         * seem to be a real-world use-case for supporting it. */
        if (ofp_port == OFPP_CONTROLLER) {
            VLOG_WARN_RL(&rl, "unsupported controller slave");
            return OFPERR_OFPBAC_BAD_OUT_PORT;
        }
    }

    return 0;
}


/* Helper for bundle_parse and bundle_parse_load.
 *
 * Returns NULL if successful, otherwise a malloc()'d string describing the
 * error.  The caller is responsible for freeing the returned string.*/
static char * OVS_WARN_UNUSED_RESULT
bundle_parse__(const char *s, const struct ofputil_port_map *port_map,
               char **save_ptr,
               const char *fields, const char *basis, const char *algorithm,
               const char *slave_type, const char *dst,
               const char *slave_delim, struct ofpbuf *ofpacts)
{
    struct ofpact_bundle *bundle;

    if (!slave_delim) {
        return xasprintf("%s: not enough arguments to bundle action", s);
    }

    if (strcasecmp(slave_delim, "slaves")) {
        return xasprintf("%s: missing slave delimiter, expected `slaves' "
                         "got `%s'", s, slave_delim);
    }

    bundle = ofpact_put_BUNDLE(ofpacts);

    for (;;) {
        ofp_port_t slave_port;
        char *slave;

        slave = strtok_r(NULL, ", []", save_ptr);
        if (!slave || bundle->n_slaves >= BUNDLE_MAX_SLAVES) {
            break;
        }

        if (!ofputil_port_from_string(slave, port_map, &slave_port)) {
            return xasprintf("%s: bad port number", slave);
        }
        ofpbuf_put(ofpacts, &slave_port, sizeof slave_port);

        bundle = ofpacts->header;
        bundle->n_slaves++;
    }

    if (ofpbuf_oversized(ofpacts)) {
        return xasprintf("input too big");
    }

    ofpact_finish_BUNDLE(ofpacts, &bundle);
    bundle->basis = atoi(basis);

    if (!strcasecmp(fields, "eth_src")) {
        bundle->fields = NX_HASH_FIELDS_ETH_SRC;
    } else if (!strcasecmp(fields, "symmetric_l4")) {
        bundle->fields = NX_HASH_FIELDS_SYMMETRIC_L4;
    } else if (!strcasecmp(fields, "symmetric_l3l4")) {
        bundle->fields = NX_HASH_FIELDS_SYMMETRIC_L3L4;
    } else if (!strcasecmp(fields, "symmetric_l3l4+udp")) {
        bundle->fields = NX_HASH_FIELDS_SYMMETRIC_L3L4_UDP;
    } else if (!strcasecmp(fields, "nw_src")) {
        bundle->fields = NX_HASH_FIELDS_NW_SRC;
    } else if (!strcasecmp(fields, "nw_dst")) {
        bundle->fields = NX_HASH_FIELDS_NW_DST;
    } else if (!strcasecmp(fields, "symmetric_l3")) {
        bundle->fields = NX_HASH_FIELDS_SYMMETRIC_L3;
    } else {
        return xasprintf("%s: unknown fields `%s'", s, fields);
    }

    if (!strcasecmp(algorithm, "active_backup")) {
        bundle->algorithm = NX_BD_ALG_ACTIVE_BACKUP;
    } else if (!strcasecmp(algorithm, "hrw")) {
        bundle->algorithm = NX_BD_ALG_HRW;
    } else {
        return xasprintf("%s: unknown algorithm `%s'", s, algorithm);
    }

    if (strcasecmp(slave_type, "ofport")) {
        return xasprintf("%s: unknown slave_type `%s'", s, slave_type);
    }

    if (dst) {
        char *error = mf_parse_subfield(&bundle->dst, dst);
        if (error) {
            return error;
        }

        if (!mf_nxm_header(bundle->dst.field->id)) {
            return xasprintf("%s: experimenter OXM field '%s' not supported",
                             s, dst);
        }
    }

    return NULL;
}

/* Converts a bundle action string contained in 's' to an nx_action_bundle and
 * stores it in 'b'.  Sets 'b''s l2 pointer to NULL.
 *
 * Returns NULL if successful, otherwise a malloc()'d string describing the
 * error.  The caller is responsible for freeing the returned string. */
char * OVS_WARN_UNUSED_RESULT
bundle_parse(const char *s, const struct ofputil_port_map *port_map,
             struct ofpbuf *ofpacts)
{
    char *fields, *basis, *algorithm, *slave_type, *slave_delim;
    char *tokstr, *save_ptr;
    char *error;

    save_ptr = NULL;
    tokstr = xstrdup(s);
    fields = strtok_r(tokstr, ", ", &save_ptr);
    basis = strtok_r(NULL, ", ", &save_ptr);
    algorithm = strtok_r(NULL, ", ", &save_ptr);
    slave_type = strtok_r(NULL, ", ", &save_ptr);
    slave_delim = strtok_r(NULL, ": ", &save_ptr);

    error = bundle_parse__(s, port_map,
                           &save_ptr, fields, basis, algorithm, slave_type,
                           NULL, slave_delim, ofpacts);
    free(tokstr);

    return error;
}

/* Converts a bundle_load action string contained in 's' to an nx_action_bundle
 * and stores it in 'b'.  Sets 'b''s l2 pointer to NULL.
 *
 * Returns NULL if successful, otherwise a malloc()'d string describing the
 * error.  The caller is responsible for freeing the returned string.*/
char * OVS_WARN_UNUSED_RESULT
bundle_parse_load(const char *s, const struct ofputil_port_map *port_map,
                  struct ofpbuf *ofpacts)
{
    char *fields, *basis, *algorithm, *slave_type, *dst, *slave_delim;
    char *tokstr, *save_ptr;
    char *error;

    save_ptr = NULL;
    tokstr = xstrdup(s);
    fields = strtok_r(tokstr, ", ", &save_ptr);
    basis = strtok_r(NULL, ", ", &save_ptr);
    algorithm = strtok_r(NULL, ", ", &save_ptr);
    slave_type = strtok_r(NULL, ", ", &save_ptr);
    dst = strtok_r(NULL, ", ", &save_ptr);
    slave_delim = strtok_r(NULL, ": ", &save_ptr);

    error = bundle_parse__(s, port_map,
                           &save_ptr, fields, basis, algorithm, slave_type,
                           dst, slave_delim, ofpacts);

    free(tokstr);

    return error;
}

/* Appends a human-readable representation of 'nab' to 's'.  If 'port_map' is
 * nonnull, uses it to translate port numbers to names in output. */
void
bundle_format(const struct ofpact_bundle *bundle,
              const struct ofputil_port_map *port_map, struct ds *s)
{
    const char *action, *fields, *algorithm;
    size_t i;

    fields = flow_hash_fields_to_str(bundle->fields);

    switch (bundle->algorithm) {
    case NX_BD_ALG_HRW:
        algorithm = "hrw";
        break;
    case NX_BD_ALG_ACTIVE_BACKUP:
        algorithm = "active_backup";
        break;
    default:
        algorithm = "<unknown>";
    }

    action = bundle->dst.field ? "bundle_load" : "bundle";

    ds_put_format(s, "%s%s(%s%s,%"PRIu16",%s,%s,", colors.paren, action,
                  colors.end, fields, bundle->basis, algorithm, "ofport");

    if (bundle->dst.field) {
        mf_format_subfield(&bundle->dst, s);
        ds_put_char(s, ',');
    }

    ds_put_format(s, "%sslaves:%s", colors.param, colors.end);
    for (i = 0; i < bundle->n_slaves; i++) {
        if (i) {
            ds_put_char(s, ',');
        }

        ofputil_format_port(bundle->slaves[i], port_map, s);
    }

    ds_put_format(s, "%s)%s", colors.paren, colors.end);
}
