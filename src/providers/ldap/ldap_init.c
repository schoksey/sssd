/*
    SSSD

    LDAP Provider Initialization functions

    Authors:
        Simo Sorce <ssorce@redhat.com>

    Copyright (C) 2009 Red Hat

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "providers/child_common.h"
#include "providers/ldap/ldap_common.h"
#include "providers/ldap/sdap_async_private.h"
#include "providers/ldap/sdap_access.h"

static void sdap_shutdown(struct be_req *req);

/* Id Handler */
struct bet_ops sdap_id_ops = {
    .handler = sdap_account_info_handler,
    .finalize = sdap_shutdown,
    .check_online = sdap_check_online
};

/* Auth Handler */
struct bet_ops sdap_auth_ops = {
    .handler = sdap_pam_auth_handler,
    .finalize = sdap_shutdown
};

/* Chpass Handler */
struct bet_ops sdap_chpass_ops = {
    .handler = sdap_pam_chpass_handler,
    .finalize = sdap_shutdown
};

/* Access Handler */
struct bet_ops sdap_access_ops = {
    .handler = sdap_pam_access_handler,
    .finalize = sdap_shutdown
};

/* Please use this only for short lists */
errno_t check_order_list_for_duplicates(char **list, size_t len,
                                        bool case_sensitive)
{
    size_t c;
    size_t d;
    int cmp;

    for (c = 0; list[c] != NULL; c++) {
        for (d = c + 1; list[d] != NULL; d++) {
            if (case_sensitive) {
                cmp = strcmp(list[c], list[d]);
            } else {
                cmp = strcasecmp(list[c], list[d]);
            }
            if (cmp == 0) {
                DEBUG(1, ("Duplicate string [%s] found.\n", list[c]));
                return EINVAL;
            }
        }
    }

    return EOK;
}

int sssm_ldap_id_init(struct be_ctx *bectx,
                      struct bet_ops **ops,
                      void **pvt_data)
{
    struct sdap_id_ctx *ctx;
    const char *urls;
    const char *dns_service_name;
    const char *sasl_mech;
    int ret;

    /* If we're already set up, just return that */
    if(bectx->bet_info[BET_ID].mod_name &&
       strcmp("ldap", bectx->bet_info[BET_ID].mod_name) == 0) {
        DEBUG(8, ("Re-using sdap_id_ctx for this provider\n"));
        *ops = bectx->bet_info[BET_ID].bet_ops;
        *pvt_data = bectx->bet_info[BET_ID].pvt_bet_data;
        return EOK;
    }

    ctx = talloc_zero(bectx, struct sdap_id_ctx);
    if (!ctx) return ENOMEM;

    ctx->be = bectx;

    ret = ldap_get_options(ctx, bectx->cdb,
                           bectx->conf_path, &ctx->opts);
    if (ret != EOK) {
        goto done;
    }

    dns_service_name = dp_opt_get_string(ctx->opts->basic,
                                         SDAP_DNS_SERVICE_NAME);
    DEBUG(7, ("Service name for discovery set to %s\n", dns_service_name));

    urls = dp_opt_get_string(ctx->opts->basic, SDAP_URI);
    if (!urls) {
        DEBUG(1, ("Missing ldap_uri, will use service discovery\n"));
    }

    ret = sdap_service_init(ctx, ctx->be, "LDAP",
                            dns_service_name, urls, &ctx->service);
    if (ret != EOK) {
        DEBUG(1, ("Failed to initialize failover service!\n"));
        goto done;
    }

    sasl_mech = dp_opt_get_string(ctx->opts->basic, SDAP_SASL_MECH);
    if (sasl_mech && strcasecmp(sasl_mech, "GSSAPI") == 0) {
        if (dp_opt_get_bool(ctx->opts->basic, SDAP_KRB5_KINIT)) {
            ret = sdap_gssapi_init(ctx, ctx->opts->basic,
                                   ctx->be, ctx->service,
                                   &ctx->krb5_service);
            if (ret !=  EOK) {
                DEBUG(1, ("sdap_gssapi_init failed [%d][%s].\n",
                            ret, strerror(ret)));
                goto done;
            }
        }
    }

    ret = setup_tls_config(ctx->opts->basic);
    if (ret != EOK) {
        DEBUG(1, ("setup_tls_config failed [%d][%s].\n",
                  ret, strerror(ret)));
        goto done;
    }

    ret = sdap_id_conn_cache_create(ctx, ctx->be,
                                    ctx->opts, ctx->service,
                                    &ctx->conn_cache);
    if (ret != EOK) {
        goto done;
    }

    ret = sdap_id_setup_tasks(ctx);
    if (ret != EOK) {
        goto done;
    }

    ret = setup_child(ctx);
    if (ret != EOK) {
        DEBUG(1, ("setup_child failed [%d][%s].\n",
                  ret, strerror(ret)));
        goto done;
    }

    *ops = &sdap_id_ops;
    *pvt_data = ctx;
    ret = EOK;

done:
    if (ret != EOK) {
        talloc_free(ctx);
    }
    return ret;
}

int sssm_ldap_auth_init(struct be_ctx *bectx,
                        struct bet_ops **ops,
                        void **pvt_data)
{
    struct sdap_auth_ctx *ctx;
    const char *urls;
    const char *dns_service_name;
    int ret;

    ctx = talloc(bectx, struct sdap_auth_ctx);
    if (!ctx) return ENOMEM;

    ctx->be = bectx;

    ret = ldap_get_options(ctx, bectx->cdb,
                           bectx->conf_path, &ctx->opts);
    if (ret != EOK) {
        goto done;
    }

    dns_service_name = dp_opt_get_string(ctx->opts->basic,
                                         SDAP_DNS_SERVICE_NAME);
    DEBUG(7, ("Service name for discovery set to %s\n", dns_service_name));

    urls = dp_opt_get_string(ctx->opts->basic, SDAP_URI);
    if (!urls) {
        DEBUG(1, ("Missing ldap_uri, will use service discovery\n"));
    }

    ret = sdap_service_init(ctx, ctx->be, "LDAP", dns_service_name,
                            urls, &ctx->service);
    if (ret != EOK) {
        DEBUG(1, ("Failed to initialize failover service!\n"));
        goto done;
    }

    ret = setup_tls_config(ctx->opts->basic);
    if (ret != EOK) {
        DEBUG(1, ("setup_tls_config failed [%d][%s].\n",
                  ret, strerror(ret)));
        goto done;
    }

    *ops = &sdap_auth_ops;
    *pvt_data = ctx;
    ret = EOK;

done:
    if (ret != EOK) {
        talloc_free(ctx);
    }
    return ret;
}

int sssm_ldap_chpass_init(struct be_ctx *bectx,
                          struct bet_ops **ops,
                          void **pvt_data)
{
    int ret;

    ret = sssm_ldap_auth_init(bectx, ops, pvt_data);

    *ops = &sdap_chpass_ops;

    return ret;
}

int sssm_ldap_access_init(struct be_ctx *bectx,
                          struct bet_ops **ops,
                          void **pvt_data)
{
    int ret;
    struct sdap_access_ctx *access_ctx;
    const char *filter;
    const char *order;
    char **order_list;
    int order_list_len;
    size_t c;
    const char *dummy;

    access_ctx = talloc_zero(bectx, struct sdap_access_ctx);
    if(access_ctx == NULL) {
        ret = ENOMEM;
        goto done;
    }

    ret = sssm_ldap_id_init(bectx, ops, (void **)&access_ctx->id_ctx);
    if (ret != EOK) {
        DEBUG(1, ("sssm_ldap_id_init failed.\n"));
        goto done;
    }

    order = dp_opt_get_cstring(access_ctx->id_ctx->opts->basic,
                               SDAP_ACCESS_ORDER);
    if (order == NULL) {
        DEBUG(1, ("ldap_access_order not given, using 'filter'.\n"));
        order = "filter";
    }

    ret = split_on_separator(access_ctx, order, ',', true, &order_list,
                             &order_list_len);
    if (ret != EOK) {
        DEBUG(1, ("split_on_separator failed.\n"));
        goto done;
    }

    ret = check_order_list_for_duplicates(order_list, order_list_len, false);
    if (ret != EOK) {
        DEBUG(1, ("check_order_list_for_duplicates failed.\n"));
        goto done;
    }

    if (order_list_len -1 > LDAP_ACCESS_LAST) {
        DEBUG(1, ("Currently only [%d] different access rules are supported.\n"));
        ret = EINVAL;
        goto done;
    }

    for (c = 0; order_list[c] != NULL; c++) {
        if (strcasecmp(order_list[c], LDAP_ACCESS_FILTER_NAME) == 0) {
            access_ctx->access_rule[c] = LDAP_ACCESS_FILTER;

            filter = dp_opt_get_cstring(access_ctx->id_ctx->opts->basic,
                                                    SDAP_ACCESS_FILTER);
            if (filter == NULL) {
                /* It's okay if this is NULL. In that case we will simply act
                 * like the 'deny' provider.
                 */
                DEBUG(0, ("Warning: LDAP access rule 'filter' is set, "
                          "but no ldap_access_filter configured. "
                          "All domain users will be denied access.\n"));
            }
            else {
                if (filter[0] == '(') {
                    /* This filter is wrapped in parentheses.
                     * Pass it as-is to the openldap libraries.
                     */
                    access_ctx->filter = filter;
                }
                else {
                    /* Add parentheses around the filter */
                    access_ctx->filter = talloc_asprintf(access_ctx, "(%s)", filter);
                    if (access_ctx->filter == NULL) {
                        ret = ENOMEM;
                        goto done;
                    }
                }
            }

        } else if (strcasecmp(order_list[c], LDAP_ACCESS_EXPIRE_NAME) == 0) {
            access_ctx->access_rule[c] = LDAP_ACCESS_EXPIRE;

            dummy = dp_opt_get_cstring(access_ctx->id_ctx->opts->basic,
                                       SDAP_ACCOUNT_EXPIRE_POLICY);
            if (dummy == NULL) {
                DEBUG(0, ("Warning: LDAP access rule 'expire' is set, "
                          "but no ldap_account_expire_policy configured. "
                          "All domain users will be denied access.\n"));
            } else {
                if (strcasecmp(dummy, LDAP_ACCOUNT_EXPIRE_SHADOW) != 0) {
                    DEBUG(1, ("Unsupported LDAP account expire policy [%s].\n",
                              dummy));
                    ret = EINVAL;
                    goto done;
                }
            }
        } else {
            DEBUG(1, ("Unexpected access rule name [%s].\n", order_list[c]));
            ret = EINVAL;
            goto done;
        }
    }
    access_ctx->access_rule[c] = LDAP_ACCESS_EMPTY;
    if (c == 0) {
        DEBUG(0, ("Warning: access_provider=ldap set, "
                  "but ldap_access_order is empty. "
                  "All domain users will be denied access.\n"));
    }

    *ops = &sdap_access_ops;
    *pvt_data = access_ctx;

    ret = EOK;

done:
    if (ret != EOK) {
        talloc_free(access_ctx);
    }
    return ret;
}

static void sdap_shutdown(struct be_req *req)
{
    /* TODO: Clean up any internal data */
    sdap_handler_done(req, DP_ERR_OK, EOK, NULL);
}

