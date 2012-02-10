/*
 * libdmclient
 *
 * Copyright (C) 2012 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU Lesser General Public License,
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * David Navarro <david.navarro@intel.com>
 *
 */

/*!
 * @file omadmclient.c
 *
 * @brief Main file for the omadmclient library.  Contains code for APIs.
 *
 ******************************************************************************/

#include <stdlib.h>
#include <string.h>

#include "internals.h"


#define PRV_CHECK_SML_CALL(func)    if (SML_ERR_OK != (func)) return DMCLT_ERR_INTERNAL
#define PRV_MAX_MESSAGE_SIZE        "16384"

static void prvCreatePacket1(internals_t * internP)
{
    // this is the beginning of the session
     SmlAlertPtr_t   alertP;

    alertP = smlAllocAlert();
    if (alertP)
    {
        SmlReplacePtr_t replaceP;

        switch(internP->state)
        {
        case STATE_CLIENT_INIT:
            alertP->data = smlString2Pcdata("1201");
            break;
        case STATE_SERVER_INIT:
            alertP->data = smlString2Pcdata("1200");
            break;
        default:
            smlFreeProtoElement((basicElement_t *)alertP);
            return;
        }
        smlFreeItemList(alertP->itemList);
        alertP->itemList = NULL;

        replaceP = get_device_info(internP);
        if (replaceP)
        {
            add_element(internP, (basicElement_t *)alertP);
            add_element(internP, (basicElement_t *)replaceP);
            internP->state = STATE_IN_SESSION;
        }
        else
        {
            smlFreeProtoElement((basicElement_t *)alertP);
        }
    }
}

static SmlSyncHdrPtr_t prvGetHeader(internals_t * internP)
{
    SmlSyncHdrPtr_t headerP;

    headerP = smlAllocSyncHdr();
    if (headerP)
    {
        set_pcdata_string(headerP->version, "1.2");
        set_pcdata_string(headerP->proto, "DM/1.2");
        set_pcdata_hex(headerP->sessionID, internP->session_id);
        set_pcdata_int(headerP->msgID, internP->message_id);
        set_pcdata_int(headerP->msgID, internP->message_id);
        set_pcdata_string(headerP->target->locURI, internP->account->server_uri);
        set_pcdata_string(headerP->source->locURI, internP->account->id);
        if (OMADM_SYNCML_ERROR_AUTHENTICATION_ACCEPTED != internP->clt_auth)
        {
            headerP->cred = get_credentials(internP->account->toServerCred);
        }
        headerP->meta = smlAllocPcdata();
        if (headerP->meta)
        {
            SmlMetInfMetInfPtr_t metInfP;

            metInfP = smlAllocMetInfMetInf();
            if (metInfP)
            {
                metInfP->maxmsgsize = smlString2Pcdata(PRV_MAX_MESSAGE_SIZE);
                headerP->meta->contentType = SML_PCDATA_EXTENSION;
                headerP->meta->extension = SML_EXT_METINF;
                headerP->meta->length = 0;
                headerP->meta->content = metInfP;
            }
            else
            {
                smlFreePcdata(headerP->meta);
                headerP->meta = NULL;
            }
        }

    }

    return headerP;
}

static int prvComposeMessage(internals_t * internP)
{
    int toSend = -1;
    Ret_t result;
    SmlSyncHdrPtr_t syncHdrP;
    elemCell_t * cell;

    internP->message_id++;
    internP->command_id = 1;

    syncHdrP = prvGetHeader(internP);

    result = smlStartMessageExt(internP->smlH, syncHdrP, SML_VERS_1_2);

    cell = internP->elem_first;
    while(cell && result == SML_ERR_OK)
    {
        set_pcdata_int(cell->element->cmdID, internP->command_id++);
        cell->msg_id = internP->message_id;

        switch (cell->element->elementType)
        {
        case SML_PE_ALERT:
            result = smlAlertCmd(internP->smlH, (SmlAlertPtr_t)(cell->element));
            toSend = 1;
            break;

        case SML_PE_REPLACE:
            result = smlReplaceCmd(internP->smlH, (SmlReplacePtr_t)(cell->element));
            toSend = 1;
            break;

        case SML_PE_RESULTS:
            result = smlResultsCmd(internP->smlH, (SmlResultsPtr_t)(cell->element));
            toSend = 1;
            break;

        case SML_PE_STATUS:
            result = smlStatusCmd(internP->smlH, (SmlStatusPtr_t)(cell->element));
            toSend++;
            break;

        default:
            // should not happen
            break;
        }

        cell = cell->next;
    }

    if (result != SML_ERR_OK)
    {
        return DMCLT_ERR_INTERNAL;
    }

    PRV_CHECK_SML_CALL(smlEndMessage(internP->smlH, SmlFinal_f));

    refresh_elements(internP);

    if (toSend <= 0)
    {
        return DMCLT_ERR_END;
    }

    return DMCLT_ERR_NONE;
}

static void prvFreeAuth(authDesc_t * authP)
{
    if (!authP) return;

    if (authP->name) free(authP->name);
    if (authP->secret) free(authP->secret);
    if (authP->data.buffer) free(authP->data.buffer);

    free(authP);
}

dmclt_err_t omadmclient_session_init(dmclt_session * sessionHP,
                                     char * flags,
                                     dmclt_callback_t UICallbacksP,
                                     void * userData)
{
    internals_t *        internP;
    SmlInstanceOptions_t options;
    SmlCallbacksPtr_t    callbacksP;

    if (sessionHP == NULL)
    {
        return DMCLT_ERR_USAGE;
    }
    *sessionHP = NULL;

    internP = (internals_t *)malloc(sizeof(internals_t));
    if (!internP)
    {
        return DMCLT_ERR_MEMORY;
    }

    memset(internP, 0, sizeof(internals_t));

    memset(&options, 0, sizeof(options));
    if (NULL != flags && (*flags)&DMCLT_FLAG_WBXML)
    {
        options.encoding= SML_WBXML;
    }
    else
    {
        options.encoding= SML_XML;
    }
    options.workspaceSize= PRV_MAX_WORKSPACE_SIZE;

    callbacksP = get_callbacks();

    if (SML_ERR_OK != smlInitInstance(callbacksP, &options, NULL, &(internP->smlH)))
    {
        goto error;
    }

    internP->alert_cb = UICallbacksP;
    internP->cb_data = userData;

    *sessionHP = (dmclt_session)internP;

    return DMCLT_ERR_NONE;

error:
    omadmclient_session_close((void**)internP);
    free(internP);
    *sessionHP = NULL;
    return DMCLT_ERR_INTERNAL;
}

dmclt_err_t omadmclient_session_add_mo(dmclt_session sessionH,
                                       omadm_mo_interface_t * moP)
{    internals_t * internP = (internals_t *)sessionH;

    if (internP == NULL || moP == NULL)
    {
        return DMCLT_ERR_USAGE;
    }

    if (OMADM_SYNCML_ERROR_NONE != momgr_add_plugin(&(internP->dmtreeH->MOs), moP, NULL))
    {
        return DMCLT_ERR_INTERNAL;
    }

    return DMCLT_ERR_NONE;
}

dmclt_err_t omadmclient_session_open(dmclt_session sessionH,
                                     char * serverID,
                                     int sessionID,
                                     char * flags)
{
    internals_t * internP = (internals_t *)sessionH;

    if (internP == NULL || serverID == NULL)
    {
        return DMCLT_ERR_USAGE;
    }

    if (OMADM_SYNCML_ERROR_NONE != dmtree_open(serverID, &(internP->dmtreeH)))
    {
        return DMCLT_ERR_INTERNAL;
    }
    if (OMADM_SYNCML_ERROR_NONE != get_server_account(internP->dmtreeH->MOs, serverID, &(internP->account)))
    {
        return DMCLT_ERR_INTERNAL;
    }

    if (NULL == internP->account->toClientCred)
    {
        internP->srv_auth = OMADM_SYNCML_ERROR_AUTHENTICATION_ACCEPTED;
    }
    if (NULL == internP->account->toServerCred)
    {
        internP->clt_auth = OMADM_SYNCML_ERROR_AUTHENTICATION_ACCEPTED;
    }

    internP->session_id = sessionID;
    internP->message_id = 0;
    if (NULL == flags || (*flags)&DMCLT_FLAG_CLIENT_INIT)
    {
        internP->state = STATE_CLIENT_INIT;
    }
    else
    {
        internP->state = STATE_SERVER_INIT;
    }

    return DMCLT_ERR_NONE;
}

dmclt_err_t omadmclient_session_open_on_alert(dmclt_session sessionH,
                                              uint8_t * pkg0,
                                              int pkg0_len,
                                              char * flags)
{
    internals_t * internP = (internals_t *)sessionH;
    char * serverID;
    int sessionID;
    dmclt_err_t err;
    buffer_t package;

    if (internP == NULL || pkg0 == NULL || pkg0_len <= 0)
    {
        return DMCLT_ERR_USAGE;
    }

    package.buffer = pkg0;
    package.len = pkg0_len;

    if (OMADM_SYNCML_ERROR_NONE != decode_package_0(package, &serverID, &sessionID, flags))
    {
        return DMCLT_ERR_USAGE;
    }

    // We open the session now since we need to access the DM tree to validate the received package0.
    err = omadmclient_session_open(sessionH,
                                   serverID,
                                   sessionID,
                                   flags);

    if (DMCLT_ERR_NONE == err)
    {
        if (OMADM_SYNCML_ERROR_NONE != validate_package_0(internP, package))
        {
            err = DMCLT_ERR_USAGE;
        }
    }

    return err;
}

void omadmclient_session_close(dmclt_session sessionH)
{
    internals_t * internP = (internals_t *)sessionH;

    if(!internP)
    {
        return;
    }

    if (internP->dmtreeH)
    {
        dmtree_close(internP->dmtreeH);
    }
    if (internP->smlH)
    {
        smlTerminateInstance(internP->smlH);
    }
    if (internP->elem_first)
    {
        free_element_list(internP->elem_first);
    }
    if (internP->old_elem)
    {
        free_element_list(internP->old_elem);
    }
    if (internP->reply_ref)
    {
        free(internP->reply_ref);
    }
    if (internP->account)
    {
        if (internP->account->id) free(internP->account->id);
        if (internP->account->server_uri) free(internP->account->server_uri);
        if (internP->account->dmtree_uri) free(internP->account->dmtree_uri);
        prvFreeAuth(internP->account->toServerCred);
        prvFreeAuth(internP->account->toClientCred);
    }
    memset(internP, 0, sizeof(internals_t));
}

dmclt_err_t omadmclient_get_next_packet(dmclt_session sessionH,
                                        dmclt_buffer_t * packetP)
{
    internals_t * internP = (internals_t *)sessionH;
    dmclt_err_t status;

    if (!internP || !packetP || !(internP->account))
    {
        return DMCLT_ERR_USAGE;
    }

    if (STATE_IN_SESSION != internP->state)
    {
        prvCreatePacket1(internP);
    }

    status = prvComposeMessage(internP);

    memset(packetP, 0, sizeof(dmclt_buffer_t));
    if (status == DMCLT_ERR_NONE)
    {
        MemPtr_t dataP;
        MemSize_t size;

        PRV_CHECK_SML_CALL(smlLockReadBuffer(internP->smlH, &dataP, &size));

        packetP->data = (unsigned char *)malloc(size);
        if (!packetP->data) return DMCLT_ERR_MEMORY;
        memcpy(packetP->data, dataP, size);

        packetP->length = size;
        packetP->uri = strdup(internP->account->server_uri);
        PRV_CHECK_SML_CALL(smlUnlockReadBuffer(internP->smlH, size));
    }

    return status;
}

dmclt_err_t omadmclient_process_reply(dmclt_session sessionH,
                                      dmclt_buffer_t * packetP)
{
    internals_t * internP = (internals_t *)sessionH;
    MemPtr_t dataP;
    MemSize_t size;

    if (!internP || !packetP)
    {
        return DMCLT_ERR_USAGE;
    }

    PRV_CHECK_SML_CALL(smlLockWriteBuffer(internP->smlH, &dataP, &size));
    if (size >= packetP->length)
    {
        memcpy(dataP, packetP->data, packetP->length);
    }
    PRV_CHECK_SML_CALL(smlUnlockWriteBuffer(internP->smlH, packetP->length));

    PRV_CHECK_SML_CALL(smlSetUserData(internP->smlH, internP));

    PRV_CHECK_SML_CALL(smlProcessData(internP->smlH, SML_ALL_COMMANDS));

    return DMCLT_ERR_NONE;
}

void omadmclient_clean_buffer(dmclt_buffer_t * packetP)
{
    if (packetP)
    {
        if (packetP->uri)
        {
            free(packetP->uri);
        }
        if (packetP->data)
        {
            free(packetP->data);
        }
        memset(packetP, 0, sizeof(dmclt_buffer_t));
    }
}
