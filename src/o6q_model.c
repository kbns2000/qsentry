/* The address space that the suites test against. See o6q_common.h.
 * SPDX-License-Identifier: Apache-2.0 */
#include "o6q_common.h"
#include "qnx/o6q_sysinfo.h"

#include <stdio.h>
#include <string.h>

#define OBJ UA_NODEID_STRING(O6Q_NS, "o6q")
#define HASCOMPONENT UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT)
#define BASEDATAVARIABLE UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE)

/* ------------------------------------------------------ 10 ms server timer */

static void tick_cb(UA_Server *server, void *data) {
    O6QModel *m = (O6QModel *)data;
    double now = o6q_now_us();
    UA_Int32 value;
    UA_Variant var;

    pthread_mutex_lock(&m->lock);
    if(m->last_tick_us > 0) {
        double err = (now - m->last_tick_us) - O6Q_TICK_MS * 1000.0;
        m->tick_err_us[m->ticks % O6Q_TICK_HISTORY] = err < 0 ? -err : err;
        m->ticks++;
    }
    m->last_tick_us = now;
    value = ++m->counter;
    pthread_mutex_unlock(&m->lock);

    UA_Variant_setScalar(&var, &value, &UA_TYPES[UA_TYPES_INT32]);
    UA_Server_writeValue(server, UA_NODEID_STRING(O6Q_NS, "counter"), var);
}

unsigned o6q_model_ticks(O6QModel *m) {
    unsigned t;
    pthread_mutex_lock(&m->lock);
    t = m->ticks;
    pthread_mutex_unlock(&m->lock);
    return t;
}

size_t o6q_model_tick_errors(O6QModel *m, double *out, size_t max) {
    size_t n;
    pthread_mutex_lock(&m->lock);
    n = m->ticks < O6Q_TICK_HISTORY ? m->ticks : O6Q_TICK_HISTORY;
    if(n > max)
        n = max;
    for(size_t i = 0; i < n; i++)
        out[i] = m->tick_err_us[(m->ticks - 1 - i) % O6Q_TICK_HISTORY];
    pthread_mutex_unlock(&m->lock);
    return n;
}

/* ---------------------------------------------------------------- methods */

static UA_StatusCode add_cb(UA_Server *server, const UA_NodeId *sessionId, void *sessionContext,
                            const UA_NodeId *methodId, void *methodContext,
                            const UA_NodeId *objectId, void *objectContext, size_t inputSize,
                            const UA_Variant *input, size_t outputSize, UA_Variant *output) {
    UA_Int32 sum;
    (void)server; (void)sessionId; (void)sessionContext; (void)methodId;
    (void)methodContext; (void)objectId; (void)objectContext;
    if(inputSize != 2 || outputSize != 1)
        return UA_STATUSCODE_BADARGUMENTSMISSING;
    if(!UA_Variant_hasScalarType(&input[0], &UA_TYPES[UA_TYPES_INT32]) ||
       !UA_Variant_hasScalarType(&input[1], &UA_TYPES[UA_TYPES_INT32]))
        return UA_STATUSCODE_BADTYPEMISMATCH;
    sum = *(const UA_Int32 *)input[0].data + *(const UA_Int32 *)input[1].data;
    return UA_Variant_setScalarCopy(&output[0], &sum, &UA_TYPES[UA_TYPES_INT32]);
}

static UA_StatusCode fire_cb(UA_Server *server, const UA_NodeId *sessionId, void *sessionContext,
                             const UA_NodeId *methodId, void *methodContext,
                             const UA_NodeId *objectId, void *objectContext, size_t inputSize,
                             const UA_Variant *input, size_t outputSize, UA_Variant *output) {
    O6QModel *m = (O6QModel *)methodContext;
    UA_UInt16 severity = 100;
    UA_DateTime now = UA_DateTime_now();
    UA_LocalizedText message = UA_LOCALIZEDTEXT("en", "o6q test event");
    UA_String source = UA_STRING("o6q");
    UA_NodeId event;
    UA_StatusCode rc;
    (void)sessionId; (void)sessionContext; (void)methodId; (void)objectId;
    (void)objectContext; (void)outputSize; (void)output;

    if(inputSize == 1 && UA_Variant_hasScalarType(&input[0], &UA_TYPES[UA_TYPES_UINT16]))
        severity = *(const UA_UInt16 *)input[0].data;

    rc = UA_Server_createEvent(server, UA_NODEID_NUMERIC(0, UA_NS0ID_BASEEVENTTYPE), &event);
    if(rc != UA_STATUSCODE_GOOD)
        return rc;
    UA_Server_writeObjectProperty_scalar(server, event, UA_QUALIFIEDNAME(0, "Time"), &now,
                                         &UA_TYPES[UA_TYPES_DATETIME]);
    UA_Server_writeObjectProperty_scalar(server, event, UA_QUALIFIEDNAME(0, "Severity"),
                                         &severity, &UA_TYPES[UA_TYPES_UINT16]);
    UA_Server_writeObjectProperty_scalar(server, event, UA_QUALIFIEDNAME(0, "Message"),
                                         &message, &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]);
    UA_Server_writeObjectProperty_scalar(server, event, UA_QUALIFIEDNAME(0, "SourceName"),
                                         &source, &UA_TYPES[UA_TYPES_STRING]);
    rc = UA_Server_triggerEvent(server, event, UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER), NULL,
                                true);
    UA_NodeId_clear(&event);
    if(rc == UA_STATUSCODE_GOOD && m) {
        pthread_mutex_lock(&m->lock);
        m->events_fired++;
        pthread_mutex_unlock(&m->lock);
    }
    return rc;
}

/* ----------------------------------------------------- qnxinfo data source */

static UA_StatusCode qnxinfo_read(UA_Server *server, const UA_NodeId *sessionId,
                                  void *sessionContext, const UA_NodeId *nodeId,
                                  void *nodeContext, UA_Boolean includeSourceTimeStamp,
                                  const UA_NumericRange *range, UA_DataValue *value) {
    O6QSysStatic st;
    O6QSysSample sa;
    char buf[512];
    UA_String s;
    UA_StatusCode rc;
    (void)server; (void)sessionId; (void)sessionContext; (void)nodeId; (void)nodeContext;
    (void)range;

    o6q_sys_static(&st);
    o6q_sys_sample(&sa);
    snprintf(buf, sizeof(buf),
             "%s %s %s host=%s cpus=%u ram_total_mb=%llu ram_free_mb=%llu processes=%u "
             "threads=%u uptime_s=%.1f clock_period_ns=%lu cycles_per_sec=%llu",
             st.sysname, st.release, st.machine, st.hostname, st.num_cpu,
             st.ram_total / (1024ULL * 1024ULL), sa.ram_free / (1024ULL * 1024ULL),
             sa.processes, sa.threads, sa.uptime_s, st.clock_period_ns, st.cycles_per_sec);
    s = UA_STRING(buf);
    rc = UA_Variant_setScalarCopy(&value->value, &s, &UA_TYPES[UA_TYPES_STRING]);
    if(rc != UA_STATUSCODE_GOOD)
        return rc;
    value->hasValue = true;
    if(includeSourceTimeStamp) {
        value->sourceTimestamp = UA_DateTime_now();
        value->hasSourceTimestamp = true;
    }
    return UA_STATUSCODE_GOOD;
}

/* ------------------------------------------------------------------ nodes */

static UA_StatusCode add_variable(UA_Server *server, const char *name, UA_Variant value,
                                  const UA_DataType *type, UA_Byte access) {
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    attr.displayName = UA_LOCALIZEDTEXT("en", (char *)(uintptr_t)name);
    attr.value = value;
    attr.dataType = type->typeId;
    attr.valueRank = UA_VALUERANK_SCALAR;
    attr.accessLevel = access;
    return UA_Server_addVariableNode(server, UA_NODEID_STRING(O6Q_NS, (char *)(uintptr_t)name),
                                     OBJ, HASCOMPONENT,
                                     UA_QUALIFIEDNAME(O6Q_NS, (char *)(uintptr_t)name),
                                     BASEDATAVARIABLE, attr, NULL, NULL);
}

UA_StatusCode o6q_model_add(UA_Server *server, O6QModel *m) {
    UA_StatusCode rc;
    UA_Variant v;

    memset(m, 0, sizeof(*m));
    pthread_mutex_init(&m->lock, NULL);

    { /* Objects/o6q */
        UA_ObjectAttributes attr = UA_ObjectAttributes_default;
        attr.displayName = UA_LOCALIZEDTEXT("en", "o6q");
        rc = UA_Server_addObjectNode(server, OBJ, UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                                     UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                                     UA_QUALIFIEDNAME(O6Q_NS, "o6q"),
                                     UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE), attr, NULL,
                                     NULL);
        if(rc != UA_STATUSCODE_GOOD)
            return rc;
    }

    { /* answer, counter */
        UA_Int32 answer = 42, zero = 0;
        UA_Variant_setScalar(&v, &answer, &UA_TYPES[UA_TYPES_INT32]);
        rc = add_variable(server, "answer", v, &UA_TYPES[UA_TYPES_INT32],
                          UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE);
        if(rc != UA_STATUSCODE_GOOD)
            return rc;
        UA_Variant_setScalar(&v, &zero, &UA_TYPES[UA_TYPES_INT32]);
        rc = add_variable(server, "counter", v, &UA_TYPES[UA_TYPES_INT32],
                          UA_ACCESSLEVELMASK_READ);
        if(rc != UA_STATUSCODE_GOOD)
            return rc;
    }

    { /* blob */
        UA_ByteString empty = UA_BYTESTRING_NULL;
        UA_Variant_setScalar(&v, &empty, &UA_TYPES[UA_TYPES_BYTESTRING]);
        rc = add_variable(server, "blob", v, &UA_TYPES[UA_TYPES_BYTESTRING],
                          UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE);
        if(rc != UA_STATUSCODE_GOOD)
            return rc;
    }

    { /* qnxinfo */
        UA_VariableAttributes attr = UA_VariableAttributes_default;
        UA_DataSource source;
        memset(&source, 0, sizeof(source));
        source.read = qnxinfo_read;
        attr.displayName = UA_LOCALIZEDTEXT("en", "qnxinfo");
        attr.dataType = UA_TYPES[UA_TYPES_STRING].typeId;
        attr.valueRank = UA_VALUERANK_SCALAR;
        attr.accessLevel = UA_ACCESSLEVELMASK_READ;
        rc = UA_Server_addDataSourceVariableNode(server, UA_NODEID_STRING(O6Q_NS, "qnxinfo"),
                                                 OBJ, HASCOMPONENT,
                                                 UA_QUALIFIEDNAME(O6Q_NS, "qnxinfo"),
                                                 BASEDATAVARIABLE, attr, source, NULL, NULL);
        if(rc != UA_STATUSCODE_GOOD)
            return rc;
    }

    { /* add(a, b) -> sum */
        UA_Argument in[2], out;
        UA_MethodAttributes attr = UA_MethodAttributes_default;
        UA_Argument_init(&in[0]);
        UA_Argument_init(&in[1]);
        UA_Argument_init(&out);
        in[0].name = UA_STRING("a");
        in[0].dataType = UA_TYPES[UA_TYPES_INT32].typeId;
        in[0].valueRank = UA_VALUERANK_SCALAR;
        in[1].name = UA_STRING("b");
        in[1].dataType = UA_TYPES[UA_TYPES_INT32].typeId;
        in[1].valueRank = UA_VALUERANK_SCALAR;
        out.name = UA_STRING("sum");
        out.dataType = UA_TYPES[UA_TYPES_INT32].typeId;
        out.valueRank = UA_VALUERANK_SCALAR;
        attr.displayName = UA_LOCALIZEDTEXT("en", "add");
        attr.executable = true;
        attr.userExecutable = true;
        rc = UA_Server_addMethodNode(server, UA_NODEID_STRING(O6Q_NS, "add"), OBJ, HASCOMPONENT,
                                     UA_QUALIFIEDNAME(O6Q_NS, "add"), attr, add_cb, 2, in, 1,
                                     &out, NULL, NULL);
        if(rc != UA_STATUSCODE_GOOD)
            return rc;
    }

    { /* fire(severity) */
        UA_Argument in;
        UA_MethodAttributes attr = UA_MethodAttributes_default;
        UA_Argument_init(&in);
        in.name = UA_STRING("severity");
        in.dataType = UA_TYPES[UA_TYPES_UINT16].typeId;
        in.valueRank = UA_VALUERANK_SCALAR;
        attr.displayName = UA_LOCALIZEDTEXT("en", "fire");
        attr.executable = true;
        attr.userExecutable = true;
        rc = UA_Server_addMethodNode(server, UA_NODEID_STRING(O6Q_NS, "fire"), OBJ,
                                     HASCOMPONENT, UA_QUALIFIEDNAME(O6Q_NS, "fire"), attr,
                                     fire_cb, 1, &in, 0, NULL, m, NULL);
        if(rc != UA_STATUSCODE_GOOD)
            return rc;
    }

    return UA_Server_addRepeatedCallback(server, tick_cb, m, O6Q_TICK_MS, NULL);
}

/* ----------------------------------------------------------- shared helpers */

UA_StatusCode o6q_server_start_model(O6QServer *srv, O6QModel *m, unsigned port) {
    UA_ServerConfig cfg;
    UA_StatusCode rc;
    memset(srv, 0, sizeof(*srv));
    memset(&cfg, 0, sizeof(cfg));
    cfg.logging = o6q_logger();
    rc = UA_ServerConfig_setMinimal(&cfg, (UA_UInt16)port, NULL);
    if(rc != UA_STATUSCODE_GOOD)
        return rc;
    o6q_server_limits(&cfg);
    srv->server = UA_Server_newWithConfig(&cfg);
    if(!srv->server)
        return UA_STATUSCODE_BADINTERNALERROR;
    rc = o6q_model_add(srv->server, m);
    if(rc == UA_STATUSCODE_GOOD)
        rc = o6q_server_run(srv);
    if(rc != UA_STATUSCODE_GOOD)
        o6q_server_stop(srv);
    return rc;
}

UA_StatusCode o6q_read_i32(UA_Client *c, const char *name, UA_Int32 *out) {
    UA_Variant v;
    UA_StatusCode rc;
    UA_Variant_init(&v);
    rc = UA_Client_readValueAttribute(c, UA_NODEID_STRING(O6Q_NS, (char *)(uintptr_t)name), &v);
    if(rc == UA_STATUSCODE_GOOD) {
        if(UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_INT32]))
            *out = *(UA_Int32 *)v.data;
        else
            rc = UA_STATUSCODE_BADTYPEMISMATCH;
    }
    UA_Variant_clear(&v);
    return rc;
}
