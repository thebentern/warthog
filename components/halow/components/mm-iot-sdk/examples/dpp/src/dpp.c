/*
 * Copyright 2025-2026 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief DPP example application.
 *
 * This example demonstrates DPP provisioning in both Push Button and QR Code Enrollee modes.
 *
 * The mode is selected via the @c dpp.mode config key:
 * - @c "pb" (default) - Push Button mode
 * - @c "qr" - QR Code Enrollee mode
 *
 * For QR mode, a DER-encoded P-256 bootstrap private key may be provisioned at @c
 * dpp.bootstrap_key. If not set, a fresh key pair is generated automatically and the DPP URI is
 * printed on startup so it can be provided to a configurator.
 *
 * @note It is assumed that you have followed the steps in the @ref GETTING_STARTED guide and are
 * therefore familiar with how to build, flash, and monitor an application using the MM-IoT-SDK
 * framework.
 */

#include <stdio.h>
#include <string.h>
#include "mmutils.h"
#include "mmosal.h"
#include "mmwlan.h"
#include "mmconfig.h"
#include "mm_app_common.h"

/** Timeout in milliseconds to wait for DPP provisioning to complete. */
#define DPP_TIMEOUT_MS (200 * 1000)

/** Maximum length of the DPP URI string. */
#define DPP_URI_MAX_LEN 256

/**
 * Convert a DPP push button result code to its string representation.
 *
 * @param result    The enumeration value of type @c mmwlan_dpp_pb_result to be converted.
 *
 * @return          A constant string describing the result. If the input is not a recognized
 *                  enumeration value, the string "Unknown Result" is returned.
 */
static const char *mmwlan_dpp_pb_result_to_string(enum mmwlan_dpp_pb_result result)
{
    switch (result)
    {
        case MMWLAN_DPP_PB_RESULT_SUCCESS:
            return "Success";
        case MMWLAN_DPP_PB_RESULT_ERROR:
            return "Error";
        case MMWLAN_DPP_PB_RESULT_SESSION_OVERLAP:
            return "Session Overlap";
        default:
            return "Unknown Result";
    }
}

/**
 * Convert a DPP authentication failure reason to its string representation.
 *
 * @param reason    The enumeration value of type @c mmwlan_dpp_auth_failure_reason to be
 *                  converted.
 *
 * @return          A constant string describing the reason. If the input is not a recognized
 *                  enumeration value, the string "Unknown" is returned.
 */
static const char *mmwlan_dpp_auth_failure_reason_to_string(
    enum mmwlan_dpp_auth_failure_reason reason)
{
    switch (reason)
    {
        case MMWLAN_DPP_AUTH_FAILURE_NO_RESPONSE:
            return "No Response";
        case MMWLAN_DPP_AUTH_FAILURE_NO_CONFIRM:
            return "No Confirm";
        case MMWLAN_DPP_AUTH_FAILURE_INIT_FAILED:
            return "Init Failed";
        default:
            return "Unknown";
    }
}

/**
 * Convert a DPP configuration failure reason to its string representation.
 *
 * @param reason    The enumeration value of type @c mmwlan_dpp_conf_failure_reason to be
 *                  converted.
 *
 * @return          A constant string describing the reason. If the input is not a recognized
 *                  enumeration value, the string "Unknown" is returned.
 */
static const char *mmwlan_dpp_conf_failure_reason_to_string(
    enum mmwlan_dpp_conf_failure_reason reason)
{
    switch (reason)
    {
        case MMWLAN_DPP_CONF_FAILURE_RESPONSE_ERROR:
            return "Response Error";
        case MMWLAN_DPP_CONF_FAILURE_TIMEOUT:
            return "Timeout";
        case MMWLAN_DPP_CONF_FAILURE_RESULT_TIMEOUT:
            return "Result Timeout";
        default:
            return "Unknown";
    }
}

/**
 * Store received DPP credentials to mmconfig and signal completion.
 *
 * @param dpp_event Reference to dpp event argument structure.
 * @param semb      Semaphore to signal on completion.
 */
static void dpp_store_credentials(const struct mmwlan_dpp_cb_args *dpp_event,
                                  struct mmosal_semb *semb)
{
    MMOSAL_DEV_ASSERT(dpp_event->event == MMWLAN_DPP_EVT_CONF_RECEIVED);

    if ((dpp_event->args.conf_received.ssid == NULL) ||
        (dpp_event->args.conf_received.passphrase == NULL) ||
        (dpp_event->args.conf_received.ssid_len > MMWLAN_SSID_MAXLEN - 1))
    {
        mmosal_printf("Invalid/incomplete credentials provided\n");
        return;
    }

    mmosal_printf("SSID %*s, PWD %s\n",
                  dpp_event->args.conf_received.ssid_len,
                  dpp_event->args.conf_received.ssid,
                  dpp_event->args.conf_received.passphrase);

    char ssid[MMWLAN_SSID_MAXLEN];
    memcpy(ssid, dpp_event->args.conf_received.ssid, dpp_event->args.conf_received.ssid_len);
    ssid[dpp_event->args.conf_received.ssid_len] = '\0';
    mmconfig_write_string("wlan.ssid", ssid);
    mmconfig_write_string("wlan.password", dpp_event->args.conf_received.passphrase);

    (void)mmosal_semb_give(semb);
}

/**
 * DPP event handler.
 *
 * @param dpp_event Reference to dpp event argument structure.
 * @param arg       User argument - pointer to completion semaphore.
 */
static void dpp_event_handler(const struct mmwlan_dpp_cb_args *dpp_event, void *arg)
{
    struct mmosal_semb *semb = (struct mmosal_semb *)arg;

    switch (dpp_event->event)
    {
        case MMWLAN_DPP_EVT_CHIRP_STARTED:
            mmosal_printf("DPP QR: chirp started\n");
            break;

        case MMWLAN_DPP_EVT_CHIRP_STOPPED:
            mmosal_printf("DPP QR: chirp stopped\n");
            break;

        case MMWLAN_DPP_EVT_PB_STARTED:
            mmosal_printf("DPP PB: started\n");
            break;

        case MMWLAN_DPP_EVT_PB_DISCOVERY:
            mmosal_printf("DPP PB: configurator found\n");
            break;

        case MMWLAN_DPP_EVT_PB_RESULT:
            mmosal_printf("DPP PB Result: %s\n",
                          mmwlan_dpp_pb_result_to_string(dpp_event->args.pb_result.result));
            if (dpp_event->args.pb_result.result != MMWLAN_DPP_PB_RESULT_SUCCESS)
            {
                (void)mmosal_semb_give(semb);
            }
            break;

        case MMWLAN_DPP_EVT_AUTH_REQ_RX:
            mmosal_printf("DPP: auth request received\n");
            break;

        case MMWLAN_DPP_EVT_AUTH_RESP_TX:
            mmosal_printf("DPP: auth response sent\n");
            break;

        case MMWLAN_DPP_EVT_AUTH_SUCCESS:
            mmosal_printf("DPP: auth succeeded\n");
            break;

        case MMWLAN_DPP_EVT_AUTH_FAILURE:
            mmosal_printf(
                "DPP: auth failed (%s)\n",
                mmwlan_dpp_auth_failure_reason_to_string(dpp_event->args.auth_failure.reason));
            (void)mmosal_semb_give(semb);
            break;

        case MMWLAN_DPP_EVT_CONF_REQ_TX:
            mmosal_printf("DPP: config request sent\n");
            break;

        case MMWLAN_DPP_EVT_CONF_RECEIVED:
            mmosal_printf("DPP: config received\n");
            dpp_store_credentials(dpp_event, semb);
            break;

        case MMWLAN_DPP_EVT_CONF_RESULT_TX:
            mmosal_printf("DPP: config result sent\n");
            break;

        case MMWLAN_DPP_EVT_CONF_FAILED:
            mmosal_printf(
                "DPP: config failed (%s)\n",
                mmwlan_dpp_conf_failure_reason_to_string(dpp_event->args.conf_failed.reason));
            (void)mmosal_semb_give(semb);
            break;

        default:
            break;
    }
}

/**
 * Main entry point to the application. This will be invoked in a thread once operating system
 * and hardware initialization has completed. It may return, but it does not have to.
 */
void app_init(void)
{
    printf("\n\nSTA DPP Connect Example (Built " __DATE__ " " __TIME__ ")\n\n");

    struct mmosal_semb *semb = mmosal_semb_create("dpp");
    MMOSAL_ASSERT(semb);

    app_wlan_init();

    struct mmwlan_dpp_args dpp_args = MMWLAN_DPP_ARGS_INIT;

    dpp_args.dpp_event_cb_arg = semb;

    char mode[8] = "pb";
    mmconfig_read_string("dpp.mode", mode, sizeof(mode));

    if (strcasecmp(mode, "qr") == 0)
    {
        dpp_args.dpp_event_cb = dpp_event_handler;
        dpp_args.mode = MMWLAN_DPP_MODE_QR_ENROLLEE;
        dpp_args.qr.chirp_iterations = 1000;

        /* Read DER-encoded bootstrap key if configured; otherwise hostap generates a fresh
         * key pair. Key can be generated with:
         *   openssl ecparam -name prime256v1 -genkey -noout -outform DER -out bootstrap.der */
        uint8_t bootstrap_key_buf[MMWLAN_DPP_BOOTSTRAP_KEY_MAX_LEN];
        int key_len = mmconfig_read_bytes("dpp.bootstrap_key",
                                          bootstrap_key_buf,
                                          sizeof(bootstrap_key_buf),
                                          0);
        if (key_len > 0)
        {
            dpp_args.qr.bootstrap_private_key = bootstrap_key_buf;
            dpp_args.qr.bootstrap_private_key_len = (uint16_t)key_len;
        }
        else
        {
            mmosal_printf("DPP QR: no bootstrap key configured, generating ephemeral key\n");
        }

        mmosal_printf("DPP QR Enrollee Start\n");
        enum mmwlan_status status = mmwlan_dpp_start(&dpp_args);
        if (status != MMWLAN_SUCCESS)
        {
            mmosal_printf("DPP Start Failed: %u\n", status);
            goto exit;
        }

        char uri[DPP_URI_MAX_LEN];
        status = mmwlan_dpp_get_uri(uri, sizeof(uri));
        if (status != MMWLAN_SUCCESS)
        {
            mmosal_printf("Failed to get DPP URI: %u\n", status);
        }
        else
        {
            mmosal_printf("DPP URI: %s\n", uri);
        }
    }
    else
    {
        dpp_args.dpp_event_cb = dpp_event_handler;

        mmosal_printf("DPP Push Button Start\n");
        enum mmwlan_status status = mmwlan_dpp_start(&dpp_args);
        if (status != MMWLAN_SUCCESS)
        {
            mmosal_printf("DPP Start Failed: %u\n", status);
            goto exit;
        }
    }

    bool ok = mmosal_semb_wait(semb, DPP_TIMEOUT_MS);
    if (!ok)
    {
        mmosal_printf("DPP timed out\n");
    }

exit:
    mmwlan_dpp_stop();

    app_wlan_start();
}
