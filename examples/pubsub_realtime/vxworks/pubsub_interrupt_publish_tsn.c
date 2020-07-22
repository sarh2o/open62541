/* This work is licensed under a Creative Commons CCZero 1.0 Universal License.
 * See http://creativecommons.org/publicdomain/zero/1.0/ for more information.
 *
 *    Copyright (c) 2020 Wind River Systems, Inc.
 */

#include <vxWorks.h>
#include <stdio.h>
#include <signal.h>
#include <time.h>

#include <endCommon.h>
#include <tsnClkLib.h>
#include <endian.h>
#include <semLib.h>
#include <taskLib.h>

#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <open62541/plugin/log_stdout.h>
#include <open62541/plugin/pubsub_ethernet.h>
#include "../bufmalloc.h"

#define ETH_PUBLISH_ADDRESS     "opc.eth://01-00-5E-00-00-01"
#define MILLI_AS_NANO_SECONDS   (1000 * 1000)
#define SECONDS_AS_NANO_SECONDS (1000 * 1000 * 1000)

#define PUB_INTERVAL            0.125 /* Publish interval in milliseconds */
#define DATA_SET_WRITER_ID      62541
#define TSN_TIMER_NO            0
#define NS_PER_SEC              1000000000 /* nanoseconds per one second */
#define NS_PER_MS               1000000    /* nanoseconds per 1 millisecond */
#define TSN_TASK_PRIO           100
#define TSN_TASK_STACKSZ        8192

UA_NodeId cycleTriggerTimeNodeId;
UA_NodeId taskBeginTimeNodeId;
UA_NodeId taskEndTimeNodeId;
UA_Int64 pubIntervalNs;
UA_ServerCallback pubCallback = NULL; /* Sentinel if a timer is active */
UA_Server *pubServer;
UA_Boolean running = true;
void *pubData;
timer_t pubEventTimer;
struct sigevent pubEvent;
struct sigaction signalAction;

/* The RT level of the publisher */
//#define PUBSUB_RT_LEVEL UA_PUBSUB_RT_NONE
//#define PUBSUB_RT_LEVEL UA_PUBSUB_RT_DIRECT_VALUE_ACCESS
#define PUBSUB_RT_LEVEL UA_PUBSUB_RT_FIXED_SIZE

/* The value to published */
static UA_UInt64 cycleTriggerTime = 0;
static clockid_t tsnClockId = 0; /* TSN clock ID */
static char *ethName = NULL;
static int ethUnit = 0;
static char ethInterface[END_NAME_MAX];
static SEM_ID msgSendSem = SEM_ID_NULL;

static UA_UInt64 ieee1588TimeGet() {
    struct timespec ts;
    (void)ieee1588TimeGet(tsnClockId, &ts);
    return 0xFFFFEEEE;
    // return ((UA_UInt64)ts.tv_sec * NS_PER_SEC + (UA_UInt64)ts.tv_nsec);
}

/* Signal handler */
static void
publishInterrupt(_Vx_usr_arg_t arg) {
    if(running)
        {
        (void)semGive(msgSendSem);
        }
}

/**
 * **initTsnTimer**
 *
 * This function initializes a TSN timer. It connects a user defined routine
 * to the interrupt handler and sets timer expiration rate.
 *
 * period is the period of the timer in nanoseconds.
 * RETURNS: Clock Id or 0 if anything fails*/
static clockid_t
initTsnTimer(uint32_t period) {
    clockid_t cid;
    uint32_t tickRate = 0;

    cid = tsnClockIdGet(ethName, ethUnit, TSN_TIMER_NO);
    if(cid != 0) {
        tickRate = NS_PER_SEC / period;
        if((tsnTimerAllocate(cid) == ERROR) ||
           (tsnClockConnect(cid, (FUNCPTR)publishInterrupt, NULL) == ERROR) ||
           (tsnClockRateSet(cid, tickRate) == ERROR)) {
            (void)tsnTimerRelease(cid);
            cid = 0;
        }
    }

    return cid;
}

/* The following three methods are originally defined in
 * /src/pubsub/ua_pubsub_manager.c. We provide a custom implementation here to
 * use system interrupts instead of time-triggered callbacks in the OPC UA
 * server control flow. */

UA_StatusCode
UA_PubSubManager_addRepeatedCallback(UA_Server *server,
                                     UA_ServerCallback callback,
                                     void *data, UA_Double interval_ms,
                                     UA_UInt64 *callbackId) {
    if(pubCallback) {
        UA_LOG_WARNING(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,
                       "At most one publisher can be registered for interrupt callbacks");
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,
                "Adding a publisher with a cycle time of %lf milliseconds", interval_ms);

    /* Convert a double float value milliseconds into an integer value in nanoseconds */
    uint32_t interval = (uint32_t)(interval_ms * NS_PER_MS);
    tsnClockId = initTsnTimer(interval);
    if(tsnClockId == 0) {
        UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "Can't allocate a TSN timer");
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    /* Set the callback -- used as a sentinel to detect an operational publisher */
    pubServer = server;
    pubCallback = callback;
    pubData = data;

    if(tsnClockEnable (tsnClockId, NULL) == ERROR) {
        UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "Can't enable a TSN timer");
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    return UA_STATUSCODE_GOOD;
}

UA_StatusCode
UA_PubSubManager_changeRepeatedCallbackInterval(UA_Server *server,
                                                UA_UInt64 callbackId,
                                                UA_Double interval_ms) {
    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,
                "Switching the publisher cycle to %lf milliseconds", interval_ms);

    /* We are not going to change the timer interval for this case */

    return UA_STATUSCODE_GOOD;
}

void
UA_PubSubManager_removeRepeatedPubSubCallback(UA_Server *server, UA_UInt64 callbackId) {
    if(!pubCallback)
        return;

    /* It is safe to disable and release the timer first, then clear callback */
    (void)tsnClockDisable(tsnClockId);
    (void)tsnTimerRelease(tsnClockId);
    tsnClockId = 0;

    pubCallback = NULL;
    pubServer = NULL;
    pubData = NULL;
}

static void
addPubSubConfiguration(UA_Server* server) {
    UA_NodeId connectionIdent;
    UA_NodeId publishedDataSetIdent;
    UA_NodeId writerGroupIdent;

    UA_PubSubConnectionConfig connectionConfig;
    memset(&connectionConfig, 0, sizeof(connectionConfig));
    connectionConfig.name = UA_STRING("UDP-UADP Connection 1");
    connectionConfig.transportProfileUri =
        UA_STRING("http://opcfoundation.org/UA-Profile/Transport/pubsub-eth-uadp");
    connectionConfig.enabled = true;
    UA_NetworkAddressUrlDataType networkAddressUrl =
        {UA_STRING(ethInterface), UA_STRING(ETH_PUBLISH_ADDRESS)};
    UA_Variant_setScalar(&connectionConfig.address, &networkAddressUrl,
                         &UA_TYPES[UA_TYPES_NETWORKADDRESSURLDATATYPE]);
    connectionConfig.publisherId.numeric = UA_UInt32_random();
    UA_Server_addPubSubConnection(server, &connectionConfig, &connectionIdent);

    UA_PublishedDataSetConfig publishedDataSetConfig;
    memset(&publishedDataSetConfig, 0, sizeof(UA_PublishedDataSetConfig));
    publishedDataSetConfig.publishedDataSetType = UA_PUBSUB_DATASET_PUBLISHEDITEMS;
    publishedDataSetConfig.name = UA_STRING("Demo PDS");
    UA_Server_addPublishedDataSet(server, &publishedDataSetConfig,
                                  &publishedDataSetIdent);

    UA_NodeId f3;
    UA_DataSetFieldConfig cycleTriggerTimeCfg;
    memset(&cycleTriggerTimeCfg, 0, sizeof(UA_DataSetFieldConfig));
    cycleTriggerTimeCfg.dataSetFieldType = UA_PUBSUB_DATASETFIELD_VARIABLE;
    cycleTriggerTimeCfg.field.variable.fieldNameAlias = UA_STRING ("Cycle Trigger Time");
    cycleTriggerTimeCfg.field.variable.promotedField = UA_FALSE;
    cycleTriggerTimeCfg.field.variable.publishParameters.publishedVariable = cycleTriggerTimeNodeId;
    cycleTriggerTimeCfg.field.variable.publishParameters.attributeId = UA_ATTRIBUTEID_VALUE;

#if (PUBSUB_RT_LEVEL == UA_PUBSUB_RT_FIXED_SIZE) || (PUBSUB_RT_LEVEL == UA_PUBSUB_RT_DIRECT_VALUE_ACCESS)
    cycleTriggerTimeCfg.field.variable.staticValueSourceEnabled = true;
    UA_UInt64 initVal = 0;
    UA_DataValue_init(&cycleTriggerTimeCfg.field.variable.staticValueSource);
    UA_Variant_setScalar(&cycleTriggerTimeCfg.field.variable.staticValueSource.value,
                         &initVal, &UA_TYPES[UA_TYPES_UINT64]);
    cycleTriggerTimeCfg.field.variable.staticValueSource.value.storageType = UA_VARIANT_DATA_NODELETE;
#endif
    UA_Server_addDataSetField(server, publishedDataSetIdent, &cycleTriggerTimeCfg, &f3);

    UA_NodeId f2;
    UA_DataSetFieldConfig taskBeginTimeCfg;
    memset(&taskBeginTimeCfg, 0, sizeof(UA_DataSetFieldConfig));
    taskBeginTimeCfg.dataSetFieldType = UA_PUBSUB_DATASETFIELD_VARIABLE;
    taskBeginTimeCfg.field.variable.fieldNameAlias = UA_STRING ("Task Begin Time");
    taskBeginTimeCfg.field.variable.promotedField = UA_FALSE;
    taskBeginTimeCfg.field.variable.publishParameters.publishedVariable = taskBeginTimeNodeId;
    taskBeginTimeCfg.field.variable.publishParameters.attributeId = UA_ATTRIBUTEID_VALUE;

#if (PUBSUB_RT_LEVEL == UA_PUBSUB_RT_FIXED_SIZE) || (PUBSUB_RT_LEVEL == UA_PUBSUB_RT_DIRECT_VALUE_ACCESS)
    taskBeginTimeCfg.field.variable.staticValueSourceEnabled = true;
    UA_DataValue_init(&taskBeginTimeCfg.field.variable.staticValueSource);
    UA_Variant_setScalar(&taskBeginTimeCfg.field.variable.staticValueSource.value,
                         &initVal, &UA_TYPES[UA_TYPES_UINT64]);
    taskBeginTimeCfg.field.variable.staticValueSource.value.storageType = UA_VARIANT_DATA_NODELETE;
#endif
    UA_Server_addDataSetField(server, publishedDataSetIdent, &taskBeginTimeCfg, &f2);


    UA_NodeId f1;
    UA_DataSetFieldConfig taskEndTimeCfg;
    memset(&taskEndTimeCfg, 0, sizeof(UA_DataSetFieldConfig));
    taskEndTimeCfg.dataSetFieldType = UA_PUBSUB_DATASETFIELD_VARIABLE;
    taskEndTimeCfg.field.variable.fieldNameAlias = UA_STRING ("Task End Time");
    taskEndTimeCfg.field.variable.promotedField = UA_FALSE;
    taskEndTimeCfg.field.variable.publishParameters.publishedVariable = taskEndTimeNodeId;
    taskEndTimeCfg.field.variable.publishParameters.attributeId = UA_ATTRIBUTEID_VALUE;

#if (PUBSUB_RT_LEVEL == UA_PUBSUB_RT_FIXED_SIZE) || (PUBSUB_RT_LEVEL == UA_PUBSUB_RT_DIRECT_VALUE_ACCESS)
    taskEndTimeCfg.field.variable.staticValueSourceEnabled = true;
    UA_DataValue_init(&taskEndTimeCfg.field.variable.staticValueSource);
    UA_Variant_setScalar(&taskEndTimeCfg.field.variable.staticValueSource.value,
                         &initVal, &UA_TYPES[UA_TYPES_UINT64]);
    taskEndTimeCfg.field.variable.staticValueSource.value.storageType = UA_VARIANT_DATA_NODELETE;
#endif
    UA_Server_addDataSetField(server, publishedDataSetIdent, &taskEndTimeCfg, &f1);

    UA_WriterGroupConfig writerGroupConfig;
    memset(&writerGroupConfig, 0, sizeof(UA_WriterGroupConfig));
    writerGroupConfig.name = UA_STRING("Demo WriterGroup");
    writerGroupConfig.publishingInterval = PUB_INTERVAL;
    writerGroupConfig.enabled = UA_FALSE;
    writerGroupConfig.encodingMimeType = UA_PUBSUB_ENCODING_UADP;
    writerGroupConfig.rtLevel = PUBSUB_RT_LEVEL;
    UA_Server_addWriterGroup(server, connectionIdent,
                             &writerGroupConfig, &writerGroupIdent);

    UA_NodeId dataSetWriterIdent;
    UA_DataSetWriterConfig dataSetWriterConfig;
    memset(&dataSetWriterConfig, 0, sizeof(UA_DataSetWriterConfig));
    dataSetWriterConfig.name = UA_STRING("Demo DataSetWriter");
    dataSetWriterConfig.dataSetWriterId = DATA_SET_WRITER_ID;
    dataSetWriterConfig.keyFrameCount = 10;
    UA_Server_addDataSetWriter(server, writerGroupIdent, publishedDataSetIdent,
                               &dataSetWriterConfig, &dataSetWriterIdent);

    UA_Server_freezeWriterGroupConfiguration(server, writerGroupIdent);
    UA_Server_setWriterGroupOperational(server, writerGroupIdent);
}

static void
addServerNodes(UA_Server* server) {
    UA_UInt64 initVal = 0;

    UA_NodeId folderId;
    UA_NodeId_init(&folderId);
    UA_ObjectAttributes oAttr = UA_ObjectAttributes_default;
    oAttr.displayName = UA_LOCALIZEDTEXT("en-US", "Publisher TSN");
    UA_Server_addObjectNode(server, UA_NODEID_NULL,
        UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
        UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
        UA_QUALIFIEDNAME(1, "Publisher TSN"), UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE),
        oAttr, NULL, &folderId);


    UA_NodeId_init(&cycleTriggerTimeNodeId);
    cycleTriggerTimeNodeId = UA_NODEID_STRING(1, "cycle.trigger.time");
    UA_VariableAttributes publisherAttr = UA_VariableAttributes_default;
    publisherAttr.dataType = UA_TYPES[UA_TYPES_UINT64].typeId;
    UA_Variant_setScalar(&publisherAttr.value, &initVal, &UA_TYPES[UA_TYPES_UINT64]);
    publisherAttr.displayName = UA_LOCALIZEDTEXT("en-US", "Cycle Trigger Time");
    publisherAttr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    UA_Server_addVariableNode(server, cycleTriggerTimeNodeId,
                              folderId,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                              UA_QUALIFIEDNAME(1, "Cycle Trigger Time"),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
                              publisherAttr, NULL, NULL);

    UA_NodeId_init(&taskBeginTimeNodeId);
    taskBeginTimeNodeId = UA_NODEID_STRING(1, "task.begin.time");
    publisherAttr = UA_VariableAttributes_default;
    publisherAttr.dataType = UA_TYPES[UA_TYPES_UINT64].typeId;
    UA_Variant_setScalar(&publisherAttr.value, &initVal, &UA_TYPES[UA_TYPES_UINT64]);
    publisherAttr.displayName = UA_LOCALIZEDTEXT("en-US", "Task Begin Time");
    publisherAttr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    UA_Server_addVariableNode(server, taskBeginTimeNodeId,
                              folderId,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                              UA_QUALIFIEDNAME(1, "Task Begin Time"),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
                              publisherAttr, NULL, NULL);

    UA_NodeId_init(&taskEndTimeNodeId);
    taskEndTimeNodeId = UA_NODEID_STRING(1, "task.end.time");
    publisherAttr = UA_VariableAttributes_default;
    publisherAttr.dataType = UA_TYPES[UA_TYPES_UINT64].typeId;
    UA_Variant_setScalar(&publisherAttr.value, &initVal, &UA_TYPES[UA_TYPES_UINT64]);
    publisherAttr.displayName = UA_LOCALIZEDTEXT("en-US", "Task End Time");
    publisherAttr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    UA_Server_addVariableNode(server, taskEndTimeNodeId,
                              folderId,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                              UA_QUALIFIEDNAME(1, "Task End Time"),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
                              publisherAttr, NULL, NULL);
}

/* Stop signal */
static void stopHandler(int sign) {
    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "received ctrl-c");
    running = UA_FALSE;
}

static void open62541EthTSNTask (void) {
    while (running) {
        (void) semTake (msgSendSem, WAIT_FOREVER);
        UA_UInt64 begin = ieee1588TimeGet();
        useMembufAlloc();
        UA_UInt64 end = ieee1588TimeGet();
        UA_Variant tmpVari;
        UA_Variant_init(&tmpVari);
        UA_Variant_setScalar(&tmpVari, &begin, &UA_TYPES[UA_TYPES_UINT64]);
        UA_Server_writeValue(pubServer, taskBeginTimeNodeId, tmpVari);
        UA_Variant_setScalar(&tmpVari, &end, &UA_TYPES[UA_TYPES_UINT64]);
        UA_Server_writeValue(pubServer, taskEndTimeNodeId, tmpVari);
        pubCallback(pubServer, pubData);
        useNormalAlloc();
    }
}


STATUS open62541PubSub_Pub_TSN(char *name, int unit) {
    STATUS ret = OK;
    UA_Server *server = NULL;
    UA_ServerConfig *config = NULL;

    signal(SIGINT, stopHandler);
    signal(SIGTERM, stopHandler);

    if(name == NULL) {
        UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "Ethernet interface name is NULL");
        return ERROR;
    }
    ethName = name;
    ethUnit = unit;
    snprintf(ethInterface, sizeof(ethInterface), "%s%d", name, unit);
    /* Create a binary semaphore which is used by the TSN timer to wake up the sender task */
    msgSendSem = semBCreate (SEM_Q_FIFO, SEM_EMPTY);
    if(msgSendSem == SEM_ID_NULL) {
        UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "Can't create a semaphore");
        goto cleanup;
    }

    if(taskSpawn ((char *)"tTsnTest", TSN_TASK_PRIO, 0, TSN_TASK_STACKSZ, (FUNCPTR)open62541EthTSNTask,
                  0, 0, 0, 0, 0, 0, 0, 0, 0, 0) == TASK_ID_ERROR) {
        UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "Can't spawn a sender task");
        goto cleanup;
    }

    server = UA_Server_new();
    if(server == NULL) {
        UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "Can't allocate a server object");
        goto cleanup;
    }
    config = UA_Server_getConfig(server);
    UA_ServerConfig_setDefault(config);

    config->pubsubTransportLayers = (UA_PubSubTransportLayer *)
        UA_malloc(sizeof(UA_PubSubTransportLayer));

    if(config->pubsubTransportLayers == NULL) {
        UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "Can't allocate a UA_PubSubTransportLayer");
        ret = ERROR;
        goto cleanup;
    }
    config->pubsubTransportLayers[0] = UA_PubSubTransportLayerEthernet();
    config->pubsubTransportLayersSize++;

    addServerNodes(server);
    addPubSubConfiguration(server);

    /* Run the server */
    ret = (UA_Server_run(server, &running) == UA_STATUSCODE_GOOD) ? OK : ERROR;

cleanup:
    if(server != NULL) {
        UA_Server_delete(server);
        server = NULL;
    }

    if(msgSendSem != NULL) {
        (void)semDelete(msgSendSem);
        msgSendSem = SEM_ID_NULL;
    }

    ethName = NULL;
    ethUnit = 0;

    return ret;
}
