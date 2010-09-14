/*
 * Copyright (C) 2010 Frank Morgner
 *
 * This file is part of ccid.
 *
 * ccid is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * ccid is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * ccid.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "scutil.h"
#include "sm.h"
#include <arpa/inet.h>
#include <opensc/asn1.h>
#include <opensc/log.h>
#include <stdlib.h>
#include <string.h>

static const struct sc_asn1_entry c_sm_capdu[] = {
    { "Padding-content indicator followed by cryptogram",
        SC_ASN1_OCTET_STRING, SC_ASN1_CTX|0x07, SC_ASN1_OPTIONAL, NULL, NULL },
    { "Protected Le",
        SC_ASN1_OCTET_STRING, SC_ASN1_CTX|0x17, SC_ASN1_OPTIONAL, NULL, NULL },
    { "Cryptographic Checksum",
        SC_ASN1_OCTET_STRING, SC_ASN1_CTX|0x0E, SC_ASN1_OPTIONAL, NULL, NULL },
    { NULL , 0 , 0 , 0 , NULL , NULL }
};

static const struct sc_asn1_entry c_sm_rapdu[] = {
    { "Padding-content indicator followed by cryptogram" ,
        SC_ASN1_OCTET_STRING, SC_ASN1_CTX|0x07, SC_ASN1_OPTIONAL, NULL, NULL },
    { "Processing Status",
        SC_ASN1_OCTET_STRING, SC_ASN1_CTX|0x19, 0               , NULL, NULL },
    { "Cryptographic Checksum",
        SC_ASN1_OCTET_STRING, SC_ASN1_CTX|0x0E, SC_ASN1_OPTIONAL, NULL, NULL },
    { NULL, 0, 0, 0, NULL, NULL }
};

static int
add_iso_pad(const u8 *data, size_t datalen, int block_size, u8 **padded)
{
    u8 *p;
    size_t p_len;

    if (!padded)
        return SC_ERROR_INVALID_ARGUMENTS;

    /* calculate length of padded message */
    p_len = (datalen / block_size) * block_size + block_size;

    p = realloc(*padded, p_len);
    if (!p)
        return SC_ERROR_OUT_OF_MEMORY;

    if (*padded != data)
        memcpy(p, data, datalen);

    *padded = p;

    /* now add iso padding */
    memset(p + datalen, 0x80, 1);
    memset(p + datalen + 1, 0, p_len - datalen - 1);

    return p_len;
}

static int
add_padding(const struct sm_ctx *ctx, const u8 *data, size_t datalen,
        u8 **padded)
{
    u8 *p;

    switch (ctx->padding_indicator) {
        case SM_NO_PADDING:
            if (*padded != data) {
                p = realloc(*padded, datalen);
                if (!p)
                    return SC_ERROR_OUT_OF_MEMORY;
                *padded = p;
                memcpy(*padded, data, datalen);
            }
            return datalen;
        case SM_ISO_PADDING:
            return add_iso_pad(data, datalen, ctx->block_length, padded);
        default:
            return SC_ERROR_INVALID_ARGUMENTS;
    }
}

static int
no_padding(u8 padding_indicator, const u8 *data, size_t datalen)
{
    if (!datalen || !data)
        return SC_ERROR_INVALID_ARGUMENTS;

    size_t len;

    switch (padding_indicator) {
        case SM_NO_PADDING:
            len = datalen;
            break;
        case SM_ISO_PADDING:
            for (len = datalen;
                    len && (data[len-1] == 0);
                    len--) { };

            if (data[len-1] != 0x80)
                return SC_ERROR_INVALID_DATA;
            break;
        default:
            return SC_ERROR_NOT_SUPPORTED;
    }

    return len;
}

static int format_le(size_t le, struct sc_asn1_entry *le_entry,
        u8 **lebuf, size_t *le_len)
{
    u8 *p;

    if (!lebuf || !le_len)
        return SC_ERROR_INVALID_ARGUMENTS;

    p = realloc(*lebuf, *le_len);
    if (!p)
        return SC_ERROR_OUT_OF_MEMORY;

    switch (*le_len) {
        case 1:
            p[0] = le;
            break;
        case 2:
            p[0] = htons(le) >> 8;
            p[1] = htons(le) & 0xff;
            break;
        case 3:
            p[0] = 0x00;
            p[1] = htons(le) >> 8;
            p[2] = htons(le) & 0xff;
            break;
        default:
            return SC_ERROR_INVALID_ARGUMENTS;
    }
    *lebuf = p;

    sc_format_asn1_entry(le_entry, *lebuf, le_len, SC_ASN1_PRESENT);

    return SC_SUCCESS;
}

static int prefix_buf(u8 prefix, u8 *buf, size_t buflen, u8 **cat)
{
    u8 *p;

    p = realloc(*cat, buflen + 1);
    if (!p)
        return SC_ERROR_OUT_OF_MEMORY;

    if (*cat == buf) {
        memmove(p + 1, p, buflen);
    } else {
        memcpy(p + 1, buf, buflen);
    }
    p[0] = prefix;

    *cat = p;

    return buflen + 1;
}

static int format_data(sc_card_t *card, const struct sm_ctx *ctx,
        const u8 *data, size_t datalen,
        struct sc_asn1_entry *formatted_encrypted_data_entry,
        u8 **formatted_data, size_t *formatted_data_len)
{
    int r;
    u8 *pad_data = NULL;
    size_t pad_data_len;

    if (!ctx || !formatted_data || !formatted_data_len) {
        r = SC_ERROR_INVALID_ARGUMENTS;
        goto err;
    }

    r = add_padding(ctx, data, datalen, &pad_data);
    if (r < 0) {
        sc_error(card->ctx, "Could not add padding to data: %s",
                sc_strerror(r));
        goto err;
    }
    pad_data_len = r;

    if (card->ctx->debug > SC_LOG_TYPE_DEBUG)
        bin_log(card->ctx, "Data to encrypt", pad_data, pad_data_len);
    r = ctx->encrypt(card, ctx, pad_data, pad_data_len, formatted_data);
    if (r < 0) {
        sc_error(card->ctx, "Could not encrypt the data");
        goto err;
    }
    if (card->ctx->debug > SC_LOG_TYPE_DEBUG)
        bin_log(card->ctx, "Cryptogram", *formatted_data, r);

    r = prefix_buf(ctx->padding_indicator, *formatted_data, r, formatted_data);
    if (r < 0) {
        sc_error(card->ctx, "Could not prepend padding indicator to formatted "
                "data: %s", sc_strerror(r));
        goto err;
    }

    *formatted_data_len = r;
    sc_format_asn1_entry(formatted_encrypted_data_entry,
            *formatted_data, formatted_data_len, SC_ASN1_PRESENT);

    r = SC_SUCCESS;

err:
    if (pad_data) {
        sc_mem_clear(pad_data, pad_data_len);
        free(pad_data);
    }

    return r;
}

static int format_head(const struct sm_ctx *ctx, const sc_apdu_t *apdu,
        u8 **formatted_head)
{
    if (!apdu || !formatted_head)
        return SC_ERROR_INVALID_ARGUMENTS;

    u8 *p = realloc(*formatted_head, 4);
    if (!p)
        return SC_ERROR_OUT_OF_MEMORY;

    p[0] = apdu->cla;
    p[1] = apdu->ins;
    p[2] = apdu->p1;
    p[3] = apdu->p2;
    *formatted_head = p;

    return add_padding(ctx, *formatted_head, 4, formatted_head);
}

static int sm_encrypt(const struct sm_ctx *ctx, sc_card_t *card,
        const sc_apdu_t *apdu, sc_apdu_t *sm_apdu)
{
    struct sc_asn1_entry sm_capdu[4];
    u8 *p, *le = NULL, *sm_data = NULL, *fdata = NULL, *mac_data = NULL,
       *asn1 = NULL, *mac = NULL;
    size_t sm_data_len, fdata_len, mac_data_len, asn1_len, mac_len, le_len;
    int r;

    if (!apdu || !ctx || !card || !card->slot || !sm_apdu) {
        r = SC_ERROR_INVALID_ARGUMENTS;
        goto err;
    }

    if ((apdu->cla & 0x0C) == 0x0C) {
        r = SC_ERROR_INVALID_ARGUMENTS;
        sc_error(card->ctx, "Given APDU is already protected with some secure messaging");
        goto err;
    }

    sc_copy_asn1_entry(c_sm_capdu, sm_capdu);

    sm_apdu->sensitive = 0;
    sm_apdu->control = apdu->control;
    sm_apdu->flags = apdu->flags;
    sm_apdu->cla = apdu->cla|0x0C;
    sm_apdu->ins = apdu->ins;
    sm_apdu->p1 = apdu->p1;
    sm_apdu->p2 = apdu->p2;
    r = format_head(ctx, sm_apdu, &mac_data);
    if (r < 0) {
        sc_error(card->ctx, "Could not format header of SM apdu");
        goto err;
    }
    mac_data_len = r;

    /* get le and data depending on the case of the unsecure command */
    switch (apdu->cse) {
        case SC_APDU_CASE_1:
            break;
	case SC_APDU_CASE_2_SHORT:
            le_len = 1;
            r = format_le(apdu->le, sm_capdu + 1, &le, &le_len);
            if (r < 0) {
                sc_error(card->ctx, "Could not format Le of SM apdu");
                goto err;
            }
            if (card->ctx->debug > SC_LOG_TYPE_DEBUG)
                bin_log(card->ctx, "Protected Le (plain)", le, le_len);
            break;
	case SC_APDU_CASE_2_EXT:
            if (card->slot->active_protocol == SC_PROTO_T0) {
                /* T0 extended APDUs look just like short APDUs */
                le_len = 1;
                r = format_le(apdu->le, sm_capdu + 1, &le, &le_len);
                if (r < 0) {
                    sc_error(card->ctx, "Could not format Le of SM apdu");
                    goto err;
                }
            } else {
                /* in case of T1 always use 3 bytes for length */
                le_len = 3;
                r = format_le(apdu->le, sm_capdu + 1, &le, &le_len);
                if (r < 0) {
                    sc_error(card->ctx, "Could not format Le of SM apdu");
                    goto err;
                }
            }
            if (card->ctx->debug > SC_LOG_TYPE_DEBUG)
                bin_log(card->ctx, "Protected Le (plain)", le, le_len);
            break;
        case SC_APDU_CASE_3_SHORT:
        case SC_APDU_CASE_3_EXT:
            r = format_data(card, ctx, apdu->data, apdu->datalen,
                    sm_capdu + 0, &fdata, &fdata_len);
            if (r < 0) {
                sc_error(card->ctx, "Could not format data of SM apdu");
                goto err;
            }
            if (card->ctx->debug > SC_LOG_TYPE_DEBUG)
                bin_log(card->ctx, "Padding-content indicator followed by cryptogram (plain)",
                        fdata, fdata_len);
            break;
        case SC_APDU_CASE_4_SHORT:
            /* in case of T0 no Le byte is added */
            if (card->slot->active_protocol != SC_PROTO_T0) {
                le_len = 1;
                r = format_le(apdu->le, sm_capdu + 1, &le, &le_len);
                if (r < 0) {
                    sc_error(card->ctx, "Could not format Le of SM apdu");
                    goto err;
                }
                if (card->ctx->debug > SC_LOG_TYPE_DEBUG)
                    bin_log(card->ctx, "Protected Le (plain)", le, le_len);
            }

            r = format_data(card, ctx, apdu->data, apdu->datalen,
                    sm_capdu + 0, &fdata, &fdata_len);
            if (r < 0) {
                sc_error(card->ctx, "Could not format data of SM apdu");
                goto err;
            }
            if (card->ctx->debug > SC_LOG_TYPE_DEBUG)
                bin_log(card->ctx, "Padding-content indicator followed by cryptogram (plain)",
                        fdata, fdata_len);
            break;
        case SC_APDU_CASE_4_EXT:
            if (card->slot->active_protocol == SC_PROTO_T0) {
                /* again a T0 extended case 4 APDU looks just
                 * like a short APDU, the additional data is
                 * transferred using ENVELOPE and GET RESPONSE */
            } else {
                /* only 2 bytes are use to specify the length of the
                 * expected data */
                le_len = 2;
                r = format_le(apdu->le, sm_capdu + 1, &le, &le_len);
                if (r < 0) {
                    sc_error(card->ctx, "Could not format Le of SM apdu");
                    goto err;
                }
                if (card->ctx->debug > SC_LOG_TYPE_DEBUG)
                    bin_log(card->ctx, "Protected Le (plain)", le, le_len);
            }

            r = format_data(card, ctx, apdu->data, apdu->datalen,
                    sm_capdu + 0, &fdata, &fdata_len);
            if (r < 0) {
                sc_error(card->ctx, "Could not format data of SM apdu");
                goto err;
            }
            if (card->ctx->debug > SC_LOG_TYPE_DEBUG)
                bin_log(card->ctx, "Padding-content indicator followed by cryptogram (plain)",
                        fdata, fdata_len);
            break;
        default:
            sc_error(card->ctx, "Unhandled apdu case");
            r = SC_ERROR_INVALID_DATA;
            goto err;
    }


    r = sc_asn1_encode(card->ctx, sm_capdu, (u8 **) &asn1, &asn1_len);
    if (r < 0) {
        goto err;
    }
    if (asn1_len) {
        p = realloc(mac_data, mac_data_len + asn1_len);
        if (!p) {
            r = SC_ERROR_OUT_OF_MEMORY;
            goto err;
        }
        mac_data = p;
        memcpy(mac_data + mac_data_len, asn1, asn1_len);
        mac_data_len += asn1_len;
        r = add_padding(ctx, mac_data, mac_data_len, &mac_data);
        if (r < 0) {
            goto err;
        }
        mac_data_len = r;
    }
    if (card->ctx->debug > SC_LOG_TYPE_DEBUG)
        bin_log(card->ctx, "Data to authenticate", mac_data, mac_data_len);

    r = ctx->authenticate(card, ctx, mac_data, mac_data_len,
            &mac);
    if (r < 0) {
        sc_error(card->ctx, "Could not get authentication code");
        goto err;
    }
    mac_len = r;
    sc_format_asn1_entry(sm_capdu + 2, mac, &mac_len,
            SC_ASN1_PRESENT);
    if (card->ctx->debug > SC_LOG_TYPE_DEBUG)
        bin_log(card->ctx, "Cryptographic Checksum (plain)", mac, mac_len);


    /* format SM apdu */
    r = sc_asn1_encode(card->ctx, sm_capdu, (u8 **) &sm_data, &sm_data_len);
    if (r < 0)
        goto err;
    if (sm_apdu->datalen < sm_data_len) {
        sc_error(card->ctx, "Data for SM APDU too long");
        r = SC_ERROR_OUT_OF_MEMORY;
        goto err;
    }
    memcpy((u8 *) sm_apdu->data, sm_data, sm_data_len);
    sm_apdu->datalen = sm_data_len;
    sm_apdu->lc = sm_apdu->datalen;
    sm_apdu->le = 0;
    sm_apdu->cse = SC_APDU_CASE_4;
    if (card->ctx->debug >= SC_LOG_TYPE_DEBUG)
        bin_log(card->ctx, "ASN.1 encoded encrypted APDU data", sm_apdu->data, sm_apdu->datalen);

err:
    if (fdata)
        free(fdata);
    if (asn1)
        free(asn1);
    if (mac_data)
        free(mac_data);
    if (mac)
        free(mac);
    if (le)
        free(le);
    if (sm_data)
        free(sm_data);

    return r;
}

static int sm_decrypt(const struct sm_ctx *ctx, sc_card_t *card,
        const sc_apdu_t *sm_apdu, sc_apdu_t *apdu)
{
    int r;
    struct sc_asn1_entry sm_rapdu[4];
    struct sc_asn1_entry my_sm_rapdu[4];
    u8 sw[2], mac[8], fdata[SC_MAX_APDU_BUFFER_SIZE];
    size_t sw_len = sizeof sw, mac_len = sizeof mac, fdata_len = sizeof fdata,
           buf_len, asn1_len;
    const u8 *buf;
    u8 *data = NULL, *mac_data = NULL, *asn1 = NULL;

    sc_copy_asn1_entry(c_sm_rapdu, sm_rapdu);
    sc_format_asn1_entry(sm_rapdu + 0, fdata, &fdata_len, 0);
    sc_format_asn1_entry(sm_rapdu + 1, sw, &sw_len, 0);
    sc_format_asn1_entry(sm_rapdu + 2, mac, &mac_len, 0);

    r = sc_asn1_decode(card->ctx, sm_rapdu, sm_apdu->resp, sm_apdu->resplen,
            &buf, &buf_len);
    if (r < 0)
        goto err;
    if (buf_len > 0) {
        r = SC_ERROR_UNKNOWN_DATA_RECEIVED;
        goto err;
    }


    if (sm_rapdu[2].flags & SC_ASN1_PRESENT) {
        /* copy from sm_apdu to my_sm_apdu, but leave mac at default */
        sc_copy_asn1_entry(sm_rapdu, my_sm_rapdu);
        sc_copy_asn1_entry(&c_sm_rapdu[2], &my_sm_rapdu[2]);

        r = sc_asn1_encode(card->ctx, my_sm_rapdu, &asn1, &asn1_len);
        if (r < 0)
            goto err;
        r = add_padding(ctx, asn1, asn1_len, &mac_data);
        if (r < 0) {
            goto err;
        }
        
        r = ctx->verify_authentication(card, ctx, mac, mac_len,
                mac_data, r);
        if (r < 0)
            goto err;
    } else {
        sc_error(card->ctx, "Cryptographic Checksum missing");
        r = SC_ERROR_ASN1_OBJECT_NOT_FOUND;
        goto err;
    }


    if (sm_rapdu[0].flags & SC_ASN1_PRESENT) {
        if (ctx->padding_indicator != fdata[0]) {
            r = SC_ERROR_UNKNOWN_DATA_RECEIVED;
            goto err;
        }
        r = ctx->decrypt(card, ctx, fdata + 1, fdata_len - 1, &data);
        if (r < 0)
            goto err;
        buf_len = r;

        r = no_padding(ctx->padding_indicator, data, buf_len);
        if (r < 0) {
            sc_error(card->ctx, "Could not remove padding");
            goto err;
        }

        if (apdu->resplen < r) {
            sc_error(card->ctx, "Response of SM APDU too long");
            r = SC_ERROR_OUT_OF_MEMORY;
            goto err;
        }
        memcpy(apdu->resp, data, r);
        apdu->resplen = r;
    } else {
        apdu->resplen = 0;
    }

    if (sm_rapdu[1].flags & SC_ASN1_PRESENT) {
        if (sw_len != 2) {
            sc_error(card->ctx, "Length of processing status bytes must be 2");
            r = SC_ERROR_ASN1_END_OF_CONTENTS;
            goto err;
        }
        apdu->sw1 = sw[0];
        apdu->sw2 = sw[1];
    } else {
        sc_error(card->ctx, "Authenticated status bytes are missing");
        r = SC_ERROR_ASN1_OBJECT_NOT_FOUND;
        goto err;
    }

    if (card->ctx->debug >= SC_LOG_TYPE_DEBUG) {
        sc_debug(card->ctx, "Decrypted APDU sw1=%02x sw2=%02x",
                apdu->sw1, apdu->sw2);
        bin_log(card->ctx, "Decrypted APDU response data",
                apdu->resp, apdu->resplen);
    }

    /* XXX verify mac */

    r = SC_SUCCESS;

err:
    if (asn1) {
        free(asn1);
    }
    if (mac_data) {
        free(mac_data);
    }

    return r;
}

int sm_transmit_apdu(const struct sm_ctx *sctx, sc_card_t *card,
        sc_apdu_t *apdu)
{
    sc_apdu_t sm_apdu;
    u8 rbuf[SC_MAX_APDU_BUFFER_SIZE - 2], sbuf[SC_MAX_APDU_BUFFER_SIZE - 4];

    sm_apdu.data = sbuf;
    sm_apdu.datalen = sizeof sbuf;
    sm_apdu.resp = rbuf;
    sm_apdu.resplen = sizeof rbuf;

    if ((apdu->cla & 0x0C) == 0x0C) {
        sc_debug(card->ctx, "Given APDU is already protected with some secure messaging.");
        return sc_transmit_apdu(card, apdu);
    }

    if (!sctx || !sctx->active) {
        sc_debug(card->ctx, "Secure messaging disabled.");
        return sc_transmit_apdu(card, apdu);
    }

    if (sctx->pre_transmit)
        SC_TEST_RET(card->ctx, sctx->pre_transmit(card, sctx, apdu),
                "Could not complete SM specific pre transmit routine");
    SC_TEST_RET(card->ctx, sm_encrypt(sctx, card, apdu, &sm_apdu),
            "Could not encrypt APDU");
    SC_TEST_RET(card->ctx, sc_transmit_apdu(card, &sm_apdu),
            "Could not transmit SM APDU");
    if (sctx->post_transmit)
        SC_TEST_RET(card->ctx, sctx->post_transmit(card, sctx, &sm_apdu),
                "Could not complete SM specific post transmit routine");
    SC_TEST_RET(card->ctx, sm_decrypt(sctx, card, &sm_apdu, apdu),
            "Could not decrypt APDU");

    return SC_SUCCESS;
}
