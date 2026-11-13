/* Suite "pubsub": OPC UA PubSub (UADP over UDP) on the target.
 *
 * A publisher and a subscriber in one server exchange DataSets through the
 * UDP stack of the system: over the loopback address (unicast) and over a
 * multicast group on the first Ethernet-like interface.  The publisher sends
 * the 10 ms counter of the test model and the server time; the subscriber
 * writes what it receives into two other variables.
 *
 * "unicast.empty_datagram" then sends one UDP datagram without payload to the
 * port of the subscriber (change udp-empty-datagram in patches/).
 *
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef _QNX_SOURCE
#define _QNX_SOURCE
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "o6q_common.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define NODE(name) UA_NODEID_STRING(O6Q_NS, name)

#define PS_PUBLISHER_ID 2234
#define PS_WRITERGROUP_ID 100
#define PS_DATASETWRITER_ID 62541
#define PS_INTERVAL_MS 20.0
#define PS_WINDOW_MS 4000u
#define PS_MAX 1024
#define PS_GROUP "239.192.41.62"       /* a group of the organization-local scope */
#define PS_GROUP_EXAMPLES "224.0.0.22" /* the group that the upstream examples use */

/* Filled by the subscriber side, in the thread of the server. */
typedef struct {
    pthread_mutex_t lock;
    unsigned datasets;  /* DataSets that reached the target variables */
    unsigned backwards; /* a counter value below the one before */
    UA_Int32 first, last;
    double last_us;
    size_t n_gap, n_lat;
    double gap_err_us[PS_MAX]; /* |arrival interval - publishing interval| */
    double latency_us[PS_MAX]; /* publisher samples the value -> subscriber writes it */
} Probe;

typedef struct {
    UA_NodeId connection, dataset, writer_group, writer, reader_group, reader;
} Ids;

static void on_mirror_write(UA_Server *server, const UA_NodeId *sessionId, void *sessionContext,
                            const UA_NodeId *nodeId, void *nodeContext,
                            const UA_NumericRange *range, const UA_DataValue *data) {
    Probe *p = (Probe *)nodeContext;
    (void)server; (void)sessionId; (void)sessionContext; (void)nodeId; (void)range;
    if(!p || !data || !data->hasValue)
        return;
    pthread_mutex_lock(&p->lock);
    if(UA_Variant_hasScalarType(&data->value, &UA_TYPES[UA_TYPES_INT32])) {
        UA_Int32 v = *(const UA_Int32 *)data->value.data;
        double now = o6q_now_us();
        if(p->datasets == 0) {
            p->first = v;
        } else {
            if(v < p->last)
                p->backwards++;
            if(p->n_gap < PS_MAX) {
                double err = (now - p->last_us) - PS_INTERVAL_MS * 1000.0;
                p->gap_err_us[p->n_gap++] = err < 0 ? -err : err;
            }
        }
        p->last = v;
        p->last_us = now;
        p->datasets++;
    } else if(UA_Variant_hasScalarType(&data->value, &UA_TYPES[UA_TYPES_DATETIME])) {
        UA_DateTime sampled = *(const UA_DateTime *)data->value.data;
        if(p->n_lat < PS_MAX)
            p->latency_us[p->n_lat++] =
                (double)(UA_DateTime_now() - sampled) / (double)UA_DATETIME_USEC;
    }
    pthread_mutex_unlock(&p->lock);
}

static const char *state_name(UA_PubSubState s) {
    switch(s) {
    case UA_PUBSUBSTATE_DISABLED: return "Disabled";
    case UA_PUBSUBSTATE_PAUSED: return "Paused";
    case UA_PUBSUBSTATE_OPERATIONAL: return "Operational";
    case UA_PUBSUBSTATE_ERROR: return "Error";
    case UA_PUBSUBSTATE_PREOPERATIONAL: return "PreOperational";
    default: return "?";
    }
}

/* The first interface that is up, can do multicast, is not the loopback and
 * has an IPv4 address. One that is also running is preferred: sending on an
 * interface that is not fails with ENETDOWN. */
static int multicast_interface(char *name, size_t name_len, char *addr, size_t addr_len) {
    struct ifaddrs *list = NULL, *ifa;
    int pass, found = 0;
    if(getifaddrs(&list) != 0)
        return 0;
    for(pass = 0; pass < 2 && !found; pass++) {
        for(ifa = list; ifa && !found; ifa = ifa->ifa_next) {
            if(!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
                continue;
            if(!(ifa->ifa_flags & IFF_UP) || !(ifa->ifa_flags & IFF_MULTICAST) ||
               (ifa->ifa_flags & IFF_LOOPBACK))
                continue;
            if(pass == 0 && !(ifa->ifa_flags & IFF_RUNNING))
                continue;
            snprintf(name, name_len, "%s", ifa->ifa_name);
            if(!inet_ntop(AF_INET, &((struct sockaddr_in *)(void *)ifa->ifa_addr)->sin_addr,
                          addr, (socklen_t)addr_len))
                snprintf(addr, addr_len, "?");
            found = 1;
        }
    }
    freeifaddrs(list);
    return found;
}

/* Sending to a group without a named interface needs a route for the group
 * (FreeBSD and Linux return ENETUNREACH without one). connect() on a UDP
 * socket does that lookup and sends nothing. Returns 0 or the errno. */
static int group_route(const char *group) {
    struct sockaddr_in a;
    int err = 0, fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd < 0)
        return errno;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(9);
    if(inet_pton(AF_INET, group, &a.sin_addr) != 1)
        err = EINVAL;
    else if(connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0)
        err = errno;
    close(fd);
    return err;
}

/* One UDP datagram without payload to the loopback address. */
static int send_empty_datagram(unsigned port, int *err) {
    struct sockaddr_in a;
    ssize_t k;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd < 0) {
        *err = errno;
        return -1;
    }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    k = sendto(fd, "", 0, 0, (struct sockaddr *)&a, sizeof(a));
    *err = errno;
    close(fd);
    return k == 0 ? 0 : -1;
}

static unsigned probe_datasets(Probe *p) {
    unsigned n;
    pthread_mutex_lock(&p->lock);
    n = p->datasets;
    pthread_mutex_unlock(&p->lock);
    return n;
}

/* The two variables that the subscriber writes to. */
static UA_StatusCode add_mirror(UA_Server *server, char *name, const UA_DataType *type,
                                const void *initial, Probe *probe) {
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    UA_ValueCallback cb;
    UA_StatusCode rc;
    UA_Variant_setScalar(&attr.value, (void *)(uintptr_t)initial, type);
    attr.displayName = UA_LOCALIZEDTEXT("en", name);
    attr.dataType = type->typeId;
    attr.valueRank = UA_VALUERANK_SCALAR;
    attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    rc = UA_Server_addVariableNode(server, NODE(name), NODE("o6q"),
                                   UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                                   UA_QUALIFIEDNAME(O6Q_NS, name),
                                   UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attr, probe,
                                   NULL);
    if(rc != UA_STATUSCODE_GOOD)
        return rc;
    cb.onRead = NULL;
    cb.onWrite = on_mirror_write;
    return UA_Server_setVariableNode_valueCallback(server, NODE(name), cb);
}

/* Connection, PublishedDataSet with two fields, ReaderGroup + DataSetReader
 * with two target variables, WriterGroup + DataSetWriter.  *step names the
 * call that failed. */
static UA_StatusCode setup_pubsub(UA_Server *server, char *url, char *interface, Ids *ids,
                                  const char **step) {
    UA_StatusCode rc;

    { /* the transport */
        UA_PubSubConnectionConfig cc;
        UA_NetworkAddressUrlDataType address;
        memset(&cc, 0, sizeof(cc));
        UA_NetworkAddressUrlDataType_init(&address);
        if(interface)
            address.networkInterface = UA_STRING(interface);
        address.url = UA_STRING(url);
        cc.name = UA_STRING("o6q UADP connection");
        cc.transportProfileUri =
            UA_STRING("http://opcfoundation.org/UA-Profile/Transport/pubsub-udp-uadp");
        cc.enabled = true;
        UA_Variant_setScalar(&cc.address, &address, &UA_TYPES[UA_TYPES_NETWORKADDRESSURLDATATYPE]);
        cc.publisherIdType = UA_PUBLISHERIDTYPE_UINT16;
        cc.publisherId.uint16 = PS_PUBLISHER_ID;
        *step = "addPubSubConnection";
        rc = UA_Server_addPubSubConnection(server, &cc, &ids->connection);
        if(rc != UA_STATUSCODE_GOOD)
            return rc;
    }

    { /* what is published: the counter and the server time */
        UA_PublishedDataSetConfig pc;
        UA_DataSetFieldConfig fc;
        UA_AddPublishedDataSetResult pr;
        UA_DataSetFieldResult fr;
        memset(&pc, 0, sizeof(pc));
        pc.publishedDataSetType = UA_PUBSUB_DATASET_PUBLISHEDITEMS;
        pc.name = UA_STRING("o6q dataset");
        *step = "addPublishedDataSet";
        pr = UA_Server_addPublishedDataSet(server, &pc, &ids->dataset);
        if(pr.addResult != UA_STATUSCODE_GOOD)
            return pr.addResult;

        memset(&fc, 0, sizeof(fc));
        fc.dataSetFieldType = UA_PUBSUB_DATASETFIELD_VARIABLE;
        fc.field.variable.fieldNameAlias = UA_STRING("counter");
        fc.field.variable.promotedField = false;
        fc.field.variable.publishParameters.publishedVariable = NODE("counter");
        fc.field.variable.publishParameters.attributeId = UA_ATTRIBUTEID_VALUE;
        *step = "addDataSetField(counter)";
        fr = UA_Server_addDataSetField(server, ids->dataset, &fc, NULL);
        if(fr.result != UA_STATUSCODE_GOOD)
            return fr.result;

        memset(&fc, 0, sizeof(fc));
        fc.dataSetFieldType = UA_PUBSUB_DATASETFIELD_VARIABLE;
        fc.field.variable.fieldNameAlias = UA_STRING("time");
        fc.field.variable.promotedField = false;
        fc.field.variable.publishParameters.publishedVariable =
            UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER_SERVERSTATUS_CURRENTTIME);
        fc.field.variable.publishParameters.attributeId = UA_ATTRIBUTEID_VALUE;
        *step = "addDataSetField(time)";
        fr = UA_Server_addDataSetField(server, ids->dataset, &fc, NULL);
        if(fr.result != UA_STATUSCODE_GOOD)
            return fr.result;
    }

    { /* the subscriber; it opens the receiving socket */
        UA_ReaderGroupConfig gc;
        UA_DataSetReaderConfig rc_cfg;
        UA_FieldMetaData fields[2];
        UA_FieldTargetVariable targets[2];
        UA_UInt16 publisher = PS_PUBLISHER_ID;

        memset(&gc, 0, sizeof(gc));
        gc.name = UA_STRING("o6q reader group");
        *step = "addReaderGroup";
        rc = UA_Server_addReaderGroup(server, ids->connection, &gc, &ids->reader_group);
        if(rc != UA_STATUSCODE_GOOD)
            return rc;

        memset(&rc_cfg, 0, sizeof(rc_cfg));
        rc_cfg.name = UA_STRING("o6q reader");
        UA_Variant_setScalar(&rc_cfg.publisherId, &publisher, &UA_TYPES[UA_TYPES_UINT16]);
        rc_cfg.writerGroupId = PS_WRITERGROUP_ID;
        rc_cfg.dataSetWriterId = PS_DATASETWRITER_ID;
        UA_DataSetMetaDataType_init(&rc_cfg.dataSetMetaData);
        rc_cfg.dataSetMetaData.name = UA_STRING("o6q dataset");
        UA_FieldMetaData_init(&fields[0]);
        fields[0].name = UA_STRING("counter");
        fields[0].dataType = UA_TYPES[UA_TYPES_INT32].typeId;
        fields[0].builtInType = UA_NS0ID_INT32;
        fields[0].valueRank = UA_VALUERANK_SCALAR;
        UA_FieldMetaData_init(&fields[1]);
        fields[1].name = UA_STRING("time");
        fields[1].dataType = UA_TYPES[UA_TYPES_DATETIME].typeId;
        fields[1].builtInType = UA_NS0ID_DATETIME;
        fields[1].valueRank = UA_VALUERANK_SCALAR;
        rc_cfg.dataSetMetaData.fields = fields; /* copied by the server */
        rc_cfg.dataSetMetaData.fieldsSize = 2;
        *step = "addDataSetReader";
        rc = UA_Server_addDataSetReader(server, ids->reader_group, &rc_cfg, &ids->reader);
        if(rc != UA_STATUSCODE_GOOD)
            return rc;

        memset(targets, 0, sizeof(targets));
        UA_FieldTargetDataType_init(&targets[0].targetVariable);
        targets[0].targetVariable.attributeId = UA_ATTRIBUTEID_VALUE;
        targets[0].targetVariable.targetNodeId = NODE("mirror_counter");
        UA_FieldTargetDataType_init(&targets[1].targetVariable);
        targets[1].targetVariable.attributeId = UA_ATTRIBUTEID_VALUE;
        targets[1].targetVariable.targetNodeId = NODE("mirror_time");
        *step = "createTargetVariables";
        rc = UA_Server_DataSetReader_createTargetVariables(server, ids->reader, 2, targets);
        if(rc != UA_STATUSCODE_GOOD)
            return rc;

        *step = "setReaderGroupOperational";
        rc = UA_Server_setReaderGroupOperational(server, ids->reader_group);
        if(rc != UA_STATUSCODE_GOOD)
            return rc;
    }

    { /* the publisher; it opens the sending socket and starts the timer */
        UA_WriterGroupConfig wc;
        UA_DataSetWriterConfig dc;
        UA_UadpWriterGroupMessageDataType message;

        UA_UadpWriterGroupMessageDataType_init(&message);
        message.networkMessageContentMask = (UA_UadpNetworkMessageContentMask)(
            (UA_UInt32)UA_UADPNETWORKMESSAGECONTENTMASK_PUBLISHERID |
            (UA_UInt32)UA_UADPNETWORKMESSAGECONTENTMASK_GROUPHEADER |
            (UA_UInt32)UA_UADPNETWORKMESSAGECONTENTMASK_WRITERGROUPID |
            (UA_UInt32)UA_UADPNETWORKMESSAGECONTENTMASK_PAYLOADHEADER);
        memset(&wc, 0, sizeof(wc));
        wc.name = UA_STRING("o6q writer group");
        wc.publishingInterval = PS_INTERVAL_MS;
        wc.enabled = false;
        wc.writerGroupId = PS_WRITERGROUP_ID;
        wc.encodingMimeType = UA_PUBSUB_ENCODING_UADP;
        wc.messageSettings.encoding = UA_EXTENSIONOBJECT_DECODED;
        wc.messageSettings.content.decoded.type = &UA_TYPES[UA_TYPES_UADPWRITERGROUPMESSAGEDATATYPE];
        wc.messageSettings.content.decoded.data = &message; /* copied by the server */
        *step = "addWriterGroup";
        rc = UA_Server_addWriterGroup(server, ids->connection, &wc, &ids->writer_group);
        if(rc != UA_STATUSCODE_GOOD)
            return rc;

        memset(&dc, 0, sizeof(dc));
        dc.name = UA_STRING("o6q writer");
        dc.dataSetWriterId = PS_DATASETWRITER_ID;
        dc.keyFrameCount = 10;
        *step = "addDataSetWriter";
        rc = UA_Server_addDataSetWriter(server, ids->writer_group, ids->dataset, &dc, &ids->writer);
        if(rc != UA_STATUSCODE_GOOD)
            return rc;

        *step = "setWriterGroupOperational";
        rc = UA_Server_setWriterGroupOperational(server, ids->writer_group);
        if(rc != UA_STATUSCODE_GOOD)
            return rc;
    }

    *step = "";
    return UA_STATUSCODE_GOOD;
}

static void clear_ids(Ids *ids) {
    UA_NodeId_clear(&ids->connection);
    UA_NodeId_clear(&ids->dataset);
    UA_NodeId_clear(&ids->writer_group);
    UA_NodeId_clear(&ids->writer);
    UA_NodeId_clear(&ids->reader_group);
    UA_NodeId_clear(&ids->reader);
}

/* One transport: start a server, configure publisher and subscriber while the
 * server runs, let the DataSets flow, stop.  interface == NULL leaves the
 * choice of the interface to the system.  quiet_setup marks the warnings of
 * the setup as expected (upstream warns when no interface is named).
 * empty_port != 0: also send a datagram without payload to that port of the
 * loopback address and see whether the subscriber still receives. */
static void run_case(const char *name, unsigned tcp_port, char *url, char *interface,
                     int quiet_setup, unsigned empty_port) {
    static O6QModel model;
    static Probe probe;
    static double sorted[PS_MAX];
    O6QServer srv;
    UA_ServerConfig cfg;
    Ids ids;
    UA_StatusCode rc;
    const char *step = "";
    char test[64];
    int fds0 = o6q_open_fds(), fds1;
    double t0, secs;
    UA_Int32 minus_one = -1;
    UA_DateTime zero_time = 0;

    memset(&srv, 0, sizeof(srv));
    memset(&ids, 0, sizeof(ids));
    memset(&probe, 0, sizeof(probe));
    pthread_mutex_init(&probe.lock, NULL);

    snprintf(test, sizeof(test), "%s.setup", name);
    o6q_step(test);
    memset(&cfg, 0, sizeof(cfg));
    cfg.logging = o6q_logger();
    rc = UA_ServerConfig_setMinimal(&cfg, (UA_UInt16)tcp_port, NULL);
    if(rc == UA_STATUSCODE_GOOD) {
        o6q_server_limits(&cfg);
        /* The subscriber of open62541 1.4 only takes key frames, and the
         * publisher sends delta frames for a DataSet with more than one field
         * unless this is switched off. */
        cfg.pubSubConfig.enableDeltaFrames = false;
        srv.server = UA_Server_newWithConfig(&cfg);
        if(!srv.server)
            rc = UA_STATUSCODE_BADINTERNALERROR;
    }
    if(rc == UA_STATUSCODE_GOOD)
        rc = o6q_model_add(srv.server, &model);
    if(rc == UA_STATUSCODE_GOOD)
        rc = add_mirror(srv.server, "mirror_counter", &UA_TYPES[UA_TYPES_INT32], &minus_one, &probe);
    if(rc == UA_STATUSCODE_GOOD)
        rc = add_mirror(srv.server, "mirror_time", &UA_TYPES[UA_TYPES_DATETIME], &zero_time, &probe);
    if(rc == UA_STATUSCODE_GOOD)
        rc = o6q_server_run(&srv);
    if(rc != UA_STATUSCODE_GOOD) {
        CHECK(test, 0, "server for %s -> %s", url, SC(rc));
        o6q_server_stop(&srv);
        pthread_mutex_destroy(&probe.lock);
        return;
    }

    /* From here on the event loop runs in its own thread, and the PubSub
     * objects are created from this one. */
    if(quiet_setup)
        o6q_expect_errors(1);
    rc = setup_pubsub(srv.server, url, interface, &ids, &step);
    if(quiet_setup)
        o6q_expect_errors(0);
    CHECK(test, rc == UA_STATUSCODE_GOOD,
          "%s%s%s, publisher %d, writer group %d, writer %d, every %.0f ms -> %s%s%s", url,
          interface ? " on " : "", interface ? interface : "", PS_PUBLISHER_ID, PS_WRITERGROUP_ID,
          PS_DATASETWRITER_ID, PS_INTERVAL_MS, SC(rc), step[0] ? " at " : "", step);

    if(rc == UA_STATUSCODE_GOOD) {
        unsigned n, backwards, expected = (unsigned)((double)PS_WINDOW_MS / PS_INTERVAL_MS);
        size_t n_gap, n_lat;
        UA_Int32 first, last, in_space = -1;
        UA_PubSubState wg = UA_PUBSUBSTATE_ERROR, rg = UA_PUBSUBSTATE_ERROR,
                       dr = UA_PUBSUBSTATE_ERROR;
        UA_StatusCode s1, s2, s3;
        UA_Variant v;

        snprintf(test, sizeof(test), "%s.flow", name);
        o6q_step(test);
        t0 = o6q_now_us();
        o6q_sleep_ms(PS_WINDOW_MS);
        secs = (o6q_now_us() - t0) / 1e6;

        pthread_mutex_lock(&probe.lock);
        n = probe.datasets;
        backwards = probe.backwards;
        first = probe.first;
        last = probe.last;
        n_gap = probe.n_gap;
        n_lat = probe.n_lat;
        pthread_mutex_unlock(&probe.lock);

        /* Read after the snapshot: the variable can only be newer. */
        UA_Variant_init(&v);
        if(UA_Server_readValue(srv.server, NODE("mirror_counter"), &v) == UA_STATUSCODE_GOOD &&
           UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_INT32]))
            in_space = *(UA_Int32 *)v.data;
        UA_Variant_clear(&v);

        CHECK(test, n >= expected / 2 && backwards == 0 && in_space >= last && last > first,
              "%u DataSets received in %.1f s (%.1f per second, %u expected); counter %d -> %d, "
              "%u out of order; value in the address space %d",
              n, secs, (double)n / secs, expected, (int)first, (int)last, backwards,
              (int)in_space);

        /* o6q_lat sorts: work on copies, the server still writes. */
        pthread_mutex_lock(&probe.lock);
        memcpy(sorted, probe.latency_us, sizeof(double) * n_lat);
        pthread_mutex_unlock(&probe.lock);
        snprintf(test, sizeof(test), "%s_latency", name);
        o6q_lat(test, sorted, n_lat);
        pthread_mutex_lock(&probe.lock);
        memcpy(sorted, probe.gap_err_us, sizeof(double) * n_gap);
        pthread_mutex_unlock(&probe.lock);
        snprintf(test, sizeof(test), "%s_interval_error", name);
        o6q_lat(test, sorted, n_gap);

        snprintf(test, sizeof(test), "%s.states", name);
        o6q_step(test);
        s1 = UA_Server_WriterGroup_getState(srv.server, ids.writer_group, &wg);
        s2 = UA_Server_ReaderGroup_getState(srv.server, ids.reader_group, &rg);
        s3 = UA_Server_DataSetReader_getState(srv.server, ids.reader, &dr);
        CHECK(test,
              s1 == UA_STATUSCODE_GOOD && s2 == UA_STATUSCODE_GOOD && s3 == UA_STATUSCODE_GOOD &&
                  wg == UA_PUBSUBSTATE_OPERATIONAL && rg == UA_PUBSUBSTATE_OPERATIONAL &&
                  dr == UA_PUBSUBSTATE_OPERATIONAL,
              "WriterGroup %s, ReaderGroup %s, DataSetReader %s", state_name(wg), state_name(rg),
              state_name(dr));

        if(empty_port) {
            /* The release closes the receiving socket: from then on no
             * DataSet arrives and one descriptor less is open. The rate is
             * compared with the one of the flow phase, not the nominal one,
             * so that a slow emulated CPU cannot fail the port. */
            unsigned before, after, need = (unsigned)(0.5 * (double)n / secs);
            int err = 0, sent, fds_a = o6q_open_fds(), fds_b;
            if(need < 1)
                need = 1;
            snprintf(test, sizeof(test), "%s.empty_datagram", name);
            o6q_step(test);
            sent = send_empty_datagram(empty_port, &err);
            o6q_sleep_ms(200); /* whatever was on its way has arrived */
            before = probe_datasets(&probe);
            o6q_sleep_ms(1000);
            after = probe_datasets(&probe);
            fds_b = o6q_open_fds();
            if(sent != 0)
                o6q_skip(test, "cannot send a datagram without payload: %s", strerror(err));
            else
                CHECK(test, after - before >= need && fds_b == fds_a,
                      "one UDP datagram without payload sent to port %u; in the second after "
                      "that the subscriber received %u DataSets (at least %u needed, %.1f per "
                      "second in the flow phase); open descriptors %d -> %d",
                      empty_port, after - before, need, (double)n / secs, fds_a, fds_b);
        }
    }

    snprintf(test, sizeof(test), "%s.stop", name);
    o6q_step(test);
    t0 = o6q_now_us();
    o6q_server_stop(&srv);
    fds1 = o6q_open_fds();
    CHECK(test, fds1 == fds0, "server and PubSub stopped in %.0f ms; open descriptors %d -> %d",
          (o6q_now_us() - t0) / 1000.0, fds0, fds1);
    clear_ids(&ids);
    pthread_mutex_destroy(&probe.lock);
}

int o6q_suite_pubsub(unsigned port, int n) {
    char url[64], ifname[64] = "", ifaddr[64] = "";
    (void)n;

    o6q_begin("pubsub", 300);

    /* Whatever the process opens once and keeps (for example by the first
     * use of the random number generator) is open after this. */
    o6q_step("baseline");
    {
        static O6QModel model;
        O6QServer srv;
        double t0 = o6q_now_us();
        UA_StatusCode rc = o6q_server_start_model(&srv, &model, port);
        o6q_server_stop(&srv);
        CHECK("baseline", rc == UA_STATUSCODE_GOOD,
              "server without PubSub started and stopped -> %s in %.0f ms; %d descriptors open",
              SC(rc), (o6q_now_us() - t0) / 1000.0, o6q_open_fds());
        if(rc != UA_STATUSCODE_GOOD)
            return o6q_end();
    }

    snprintf(url, sizeof(url), "opc.udp://127.0.0.1:%u/", port + 10);
    run_case("unicast", port, url, NULL, 0, port + 10);

    if(!multicast_interface(ifname, sizeof(ifname), ifaddr, sizeof(ifaddr))) {
        o6q_skip("multicast.setup", "no interface that is up, has an IPv4 address and can do "
                                    "multicast (other than the loopback)");
        o6q_skip("multicast_anyif.setup", "same reason");
    } else {
        int rerr;
        o6q_info("multicast interface %s (%s), groups %s and %s", ifname, ifaddr, PS_GROUP,
                 PS_GROUP_EXAMPLES);
        snprintf(url, sizeof(url), "opc.udp://%s:%u/", PS_GROUP, port + 11);
        run_case("multicast", port, url, ifname, 0, 0);
        /* As in the upstream examples: their group, and no interface named.
         * Without a named interface the system needs a route to send. */
        rerr = group_route(PS_GROUP_EXAMPLES);
        if(rerr != 0) {
            o6q_skip("multicast_anyif.setup", "no route for %s (%s): without a named interface "
                                              "the system needs one to send", PS_GROUP_EXAMPLES,
                     strerror(rerr));
        } else {
            snprintf(url, sizeof(url), "opc.udp://%s:%u/", PS_GROUP_EXAMPLES, port + 12);
            run_case("multicast_anyif", port, url, NULL, 1, 0);
        }
    }

    return o6q_end();
}
