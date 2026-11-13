/* Suite "security": encrypted SecureChannels with the OpenSSL 3 of the QNX
 * SDP, certificate trust, user authentication.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef _QNX_SOURCE
#define _QNX_SOURCE
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "o6q_common.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The release has UA_CertificateVerification_CertFolders() for Linux only;
 * the port adds QNX (patches/10-pki-folders.change). */
#if defined(__linux__) || (defined(__QNXNTO__) && !defined(O6Q_VARIANT))
#define O6Q_HAVE_CERTFOLDERS 1
#endif

#define NODE(name) UA_NODEID_STRING(O6Q_NS, name)

#define SERVER_URI "urn:open62541.server.application" /* the default of open62541 */
#define CLIENT_URI "urn:o6q:client:trusted"
#define ROGUE_URI "urn:o6q:client:rogue"

#define USER "operator"
#define PASSWORD "qnx-8-neutrino"

typedef struct {
    UA_ByteString cert;
    UA_ByteString key;
} Identity;

typedef struct {
    const char *name;
    const char *uri;
} Policy;

static const Policy policies[] = {
    {"Basic256Sha256", "http://opcfoundation.org/UA/SecurityPolicy#Basic256Sha256"},
    {"Aes128Sha256RsaOaep", "http://opcfoundation.org/UA/SecurityPolicy#Aes128_Sha256_RsaOaep"},
    {"Aes256Sha256RsaPss", "http://opcfoundation.org/UA/SecurityPolicy#Aes256_Sha256_RsaPss"},
};
#define POLICIES (sizeof(policies) / sizeof(policies[0]))

/* A self-signed application instance certificate with a 2048 bit RSA key,
 * made on the target. */
static UA_StatusCode make_identity(const char *common_name, const char *uri, Identity *id,
                                   double *ms) {
    UA_String subject[3];
    UA_String alt[2];
    char cn[96], alt_uri[96];
    UA_UInt16 bits = 2048, days = 30;
    UA_KeyValueMap *params = UA_KeyValueMap_new();
    UA_StatusCode rc;
    double t0 = o6q_now_us();

    memset(id, 0, sizeof(*id));
    if(!params)
        return UA_STATUSCODE_BADOUTOFMEMORY;
    snprintf(cn, sizeof(cn), "CN=%s", common_name);
    snprintf(alt_uri, sizeof(alt_uri), "URI:%s", uri);
    subject[0] = UA_STRING("C=CA");
    subject[1] = UA_STRING("O=open62541-qnx");
    subject[2] = UA_STRING(cn);
    alt[0] = UA_STRING("DNS:localhost");
    alt[1] = UA_STRING(alt_uri);
    UA_KeyValueMap_setScalar(params, UA_QUALIFIEDNAME(0, "key-size-bits"), &bits,
                             &UA_TYPES[UA_TYPES_UINT16]);
    UA_KeyValueMap_setScalar(params, UA_QUALIFIEDNAME(0, "expires-in-days"), &days,
                             &UA_TYPES[UA_TYPES_UINT16]);
    rc = UA_CreateCertificate(o6q_logger(), subject, 3, alt, 2, UA_CERTIFICATEFORMAT_DER, params,
                              &id->key, &id->cert);
    UA_KeyValueMap_delete(params);
    if(ms)
        *ms = (o6q_now_us() - t0) / 1000.0;
    return rc;
}

static void clear_identity(Identity *id) {
    UA_ByteString_clear(&id->cert);
    UA_ByteString_clear(&id->key);
}

/* A client that only accepts servers from its trust list. policy_uri == NULL:
 * any policy the server offers. */
static UA_Client *secure_client(const Identity *id, const UA_ByteString *trust, size_t trust_n,
                                UA_MessageSecurityMode mode, const char *policy_uri,
                                const char *application_uri) {
    UA_ClientConfig cc;
    UA_Client *client;
    memset(&cc, 0, sizeof(cc));
    cc.logging = o6q_logger();
    if(UA_ClientConfig_setDefaultEncryption(&cc, id->cert, id->key, trust, trust_n, NULL, 0) !=
       UA_STATUSCODE_GOOD) {
        UA_ClientConfig_clear(&cc);
        return NULL;
    }
    cc.timeout = 60000; /* ms; RSA on an emulated CPU */
    cc.securityMode = mode;
    if(policy_uri)
        cc.securityPolicyUri = UA_STRING_ALLOC(policy_uri);
    /* The server compares this with the URI in the certificate. */
    UA_String_clear(&cc.clientDescription.applicationUri);
    cc.clientDescription.applicationUri = UA_STRING_ALLOC(application_uri);
    client = UA_Client_newWithConfig(&cc);
    if(!client)
        UA_ClientConfig_clear(&cc);
    return client;
}

static UA_StatusCode read_answer(UA_Client *c, UA_Int32 *out) {
    UA_Variant v;
    UA_StatusCode rc;
    UA_Variant_init(&v);
    rc = UA_Client_readValueAttribute(c, NODE("answer"), &v);
    if(rc == UA_STATUSCODE_GOOD) {
        if(UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_INT32]))
            *out = *(UA_Int32 *)v.data;
        else
            rc = UA_STATUSCODE_BADTYPEMISMATCH;
    }
    UA_Variant_clear(&v);
    return rc;
}

static void drop(UA_Client *c) {
    if(c) {
        UA_Client_disconnect(c);
        UA_Client_delete(c);
    }
}

/* One policy and mode: connect, read n times, measure. */
static void test_policy(const char *url, const Identity *client_id, const Identity *server_id,
                        const Policy *policy, UA_MessageSecurityMode mode, int n) {
    char test[64], lat[64];
    const char *mode_name = mode == UA_MESSAGESECURITYMODE_SIGN ? "Sign" : "SignAndEncrypt";
    UA_Client *c;
    UA_StatusCode rc;
    double t0, connect_ms, *samples;
    int good = 0;

    snprintf(test, sizeof(test), "%s.%s", policy->name, mode_name);
    snprintf(lat, sizeof(lat), "read_%s_%s", policy->name, mode_name);
    o6q_step(test);
    c = secure_client(client_id, &server_id->cert, 1, mode, policy->uri, CLIENT_URI);
    if(!c) {
        CHECK(test, 0, "cannot configure the client");
        return;
    }
    t0 = o6q_now_us();
    rc = UA_Client_connect(c, url);
    connect_ms = (o6q_now_us() - t0) / 1000.0;
    samples = (double *)UA_malloc(sizeof(double) * (size_t)n);
    if(rc == UA_STATUSCODE_GOOD && samples) {
        for(int i = 0; i < n; i++) {
            UA_Int32 v = 0;
            double t1 = o6q_now_us();
            UA_StatusCode r = read_answer(c, &v);
            samples[i] = o6q_now_us() - t1;
            if(r == UA_STATUSCODE_GOOD && v == 42)
                good++;
        }
    }
    CHECK(test, rc == UA_STATUSCODE_GOOD && good == n,
          "connect %s in %.0f ms (RSA-2048 handshake); %d of %d reads correct", SC(rc),
          connect_ms, good, n);
    if(rc == UA_STATUSCODE_GOOD && samples)
        o6q_lat(lat, samples, (size_t)n);
    UA_free(samples);
    drop(c);
}

/* ------------------------------------------------- certificate folders */

#ifdef O6Q_HAVE_CERTFOLDERS

typedef struct {
    char base[160];
    char trusted[200], issuers[200], crl[200];
    char client_file[260], rogue_file[260];
} PkiDirs;

/* The first place where a directory can be made. (/tmp is not always one:
 * on a small QNX image it can be a link to /dev/shmem, which has no
 * directories.) */
static int pki_dirs_make(PkiDirs *d) {
    const char *candidates[] = {getenv("TMPDIR"), "/tmp", "/data", "/var/tmp", "."};
    memset(d, 0, sizeof(*d));
    for(size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if(!candidates[i] || !candidates[i][0])
            continue;
        snprintf(d->base, sizeof(d->base), "%s/o6q_pki_%ld", candidates[i], (long)getpid());
        if(mkdir(d->base, 0700) != 0)
            continue;
        snprintf(d->trusted, sizeof(d->trusted), "%s/trusted", d->base);
        snprintf(d->issuers, sizeof(d->issuers), "%s/issuers", d->base);
        snprintf(d->crl, sizeof(d->crl), "%s/crl", d->base);
        snprintf(d->client_file, sizeof(d->client_file), "%s/client.der", d->trusted);
        snprintf(d->rogue_file, sizeof(d->rogue_file), "%s/rogue.der", d->trusted);
        if(mkdir(d->trusted, 0700) == 0 && mkdir(d->issuers, 0700) == 0 &&
           mkdir(d->crl, 0700) == 0)
            return 0;
        rmdir(d->trusted);
        rmdir(d->issuers);
        rmdir(d->crl);
        rmdir(d->base);
    }
    d->base[0] = 0;
    return -1;
}

static void pki_dirs_remove(PkiDirs *d) {
    if(!d->base[0])
        return;
    unlink(d->client_file);
    unlink(d->rogue_file);
    rmdir(d->trusted);
    rmdir(d->issuers);
    rmdir(d->crl);
    rmdir(d->base);
}

static int write_file(const char *path, const UA_ByteString *data) {
    FILE *f = fopen(path, "wb");
    size_t n;
    if(!f)
        return -1;
    n = fwrite(data->data, 1, data->length, f);
    if(fclose(f) != 0 || n != data->length)
        return -1;
    return 0;
}

static UA_StatusCode connect_read(const Identity *id, const Identity *server_id,
                                  const char *uri, const char *url) {
    UA_Client *c = secure_client(id, &server_id->cert, 1, UA_MESSAGESECURITYMODE_SIGNANDENCRYPT,
                                 policies[0].uri, uri);
    UA_Int32 v = 0;
    UA_StatusCode rc = c ? UA_Client_connect(c, url) : UA_STATUSCODE_BADOUTOFMEMORY;
    if(rc == UA_STATUSCODE_GOOD)
        rc = read_answer(c, &v);
    if(rc == UA_STATUSCODE_GOOD && v != 42)
        rc = UA_STATUSCODE_BADUNEXPECTEDERROR;
    drop(c);
    return rc;
}

/* A server whose trust list is a directory. Whoever may connect is decided
 * by the files in it, at the time of the connection. */
static void test_certfolders(unsigned port, const Identity *server_id, const Identity *client_id,
                             const Identity *rogue_id) {
    static O6QModel model;
    O6QServer srv;
    PkiDirs dirs;
    UA_ServerConfig cfg;
    UA_StatusCode rc;
    char url[64];

    memset(&srv, 0, sizeof(srv));
    snprintf(url, sizeof(url), "opc.tcp://127.0.0.1:%u", port);

    o6q_step("certfolder.startup");
    if(pki_dirs_make(&dirs) != 0) {
        o6q_skip("certfolder.startup", "no place to make a directory: %s", strerror(errno));
        return;
    }
    if(write_file(dirs.client_file, &client_id->cert) != 0) {
        o6q_skip("certfolder.startup", "cannot write %s: %s", dirs.client_file, strerror(errno));
        pki_dirs_remove(&dirs);
        return;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.logging = o6q_logger();
    /* (open62541 warns here that without a trust list every certificate is
     * accepted; the folders replace that two lines further down.) */
    o6q_expect_errors(1);
    rc = UA_ServerConfig_setDefaultWithSecurityPolicies(&cfg, (UA_UInt16)port, &server_id->cert,
                                                        &server_id->key, NULL, 0, NULL, 0, NULL,
                                                        0);
    o6q_expect_errors(0);
    if(rc == UA_STATUSCODE_GOOD)
        rc = UA_CertificateVerification_CertFolders(&cfg.secureChannelPKI, dirs.trusted,
                                                    dirs.issuers, dirs.crl);
    if(rc == UA_STATUSCODE_GOOD)
        rc = UA_CertificateVerification_CertFolders(&cfg.sessionPKI, dirs.trusted, dirs.issuers,
                                                    dirs.crl);
    /* The function clears the whole structure, its logger included. */
    cfg.secureChannelPKI.logging = cfg.logging;
    cfg.sessionPKI.logging = cfg.logging;
    if(rc == UA_STATUSCODE_GOOD) {
        o6q_server_limits(&cfg);
        srv.server = UA_Server_newWithConfig(&cfg);
        if(!srv.server)
            rc = UA_STATUSCODE_BADINTERNALERROR;
    } else {
        UA_ServerConfig_clean(&cfg);
    }
    if(rc == UA_STATUSCODE_GOOD)
        rc = o6q_model_add(srv.server, &model);
    if(rc == UA_STATUSCODE_GOOD)
        rc = o6q_server_run(&srv);
    CHECK("certfolder.startup", rc == UA_STATUSCODE_GOOD,
          "server with the trust list in the directory %s -> %s", dirs.trusted, SC(rc));
    if(rc != UA_STATUSCODE_GOOD)
        goto out;

    o6q_step("certfolder.trusted");
    rc = connect_read(client_id, server_id, CLIENT_URI, url);
    CHECK("certfolder.trusted", rc == UA_STATUSCODE_GOOD,
          "client whose certificate is a file in the directory -> %s", SC(rc));

    o6q_step("certfolder.untrusted");
    o6q_expect_errors(1);
    rc = connect_read(rogue_id, server_id, ROGUE_URI, url);
    o6q_expect_errors(0);
    CHECK("certfolder.untrusted", rc != UA_STATUSCODE_GOOD,
          "client whose certificate is not in the directory -> %s", SC(rc));

    o6q_step("certfolder.added");
    if(write_file(dirs.rogue_file, &rogue_id->cert) != 0) {
        CHECK("certfolder.added", 0, "cannot write %s: %s", dirs.rogue_file, strerror(errno));
    } else {
        rc = connect_read(rogue_id, server_id, ROGUE_URI, url);
        CHECK("certfolder.added", rc == UA_STATUSCODE_GOOD,
              "the same client after its certificate was copied into the directory, server "
              "not restarted -> %s",
              SC(rc));
    }

    o6q_step("certfolder.removed");
    unlink(dirs.rogue_file);
    o6q_expect_errors(1);
    rc = connect_read(rogue_id, server_id, ROGUE_URI, url);
    o6q_expect_errors(0);
    CHECK("certfolder.removed", rc != UA_STATUSCODE_GOOD,
          "the same client after the file was deleted again -> %s", SC(rc));

    o6q_step("certfolder.repeat");
    {
        /* Every verification reads the directories again. */
        int good = 0, rounds = 20;
        double t0 = o6q_now_us();
        for(int i = 0; i < rounds; i++) {
            if(connect_read(client_id, server_id, CLIENT_URI, url) == UA_STATUSCODE_GOOD)
                good++;
        }
        CHECK("certfolder.repeat", good == rounds,
              "%d of %d further encrypted connections of the trusted client, %.0f ms each",
              good, rounds, (o6q_now_us() - t0) / 1000.0 / rounds);
    }

out:
    o6q_step("certfolder.shutdown");
    o6q_server_stop(&srv);
    pki_dirs_remove(&dirs);
}

#endif /* O6Q_HAVE_CERTFOLDERS */

int o6q_suite_security(unsigned port, int n) {
    static O6QModel model;
    O6QServer srv;
    Identity server_id, client_id, rogue_id;
    UA_StatusCode rc, r1, r2, r3;
    char url[64];
    double ms1 = 0, ms2 = 0, ms3 = 0, t0;
    int reads = n > 0 ? n : 200;

    memset(&srv, 0, sizeof(srv));
    memset(&server_id, 0, sizeof(server_id));
    memset(&client_id, 0, sizeof(client_id));
    memset(&rogue_id, 0, sizeof(rogue_id));
    snprintf(url, sizeof(url), "opc.tcp://127.0.0.1:%u", port);

    o6q_begin("security", 1500);

    /* ---- keys and certificates, made on the target with its OpenSSL ---- */
    o6q_step("certificates");
    r1 = make_identity("o6q server", SERVER_URI, &server_id, &ms1);
    r2 = make_identity("o6q trusted client", CLIENT_URI, &client_id, &ms2);
    r3 = make_identity("o6q rogue client", ROGUE_URI, &rogue_id, &ms3);
    CHECK("certificates",
          r1 == UA_STATUSCODE_GOOD && r2 == UA_STATUSCODE_GOOD && r3 == UA_STATUSCODE_GOOD &&
              server_id.cert.length > 0 && client_id.cert.length > 0 && rogue_id.cert.length > 0,
          "three RSA-2048 keys with self-signed certificates: %s %.0f ms, %s %.0f ms, %s %.0f ms",
          SC(r1), ms1, SC(r2), ms2, SC(r3), ms3);
    if(r1 != UA_STATUSCODE_GOOD || r2 != UA_STATUSCODE_GOOD || r3 != UA_STATUSCODE_GOOD)
        goto done;

    /* ---- the server: all policies, trusts only the "trusted client" ---- */
    o6q_step("startup");
    {
        UA_ServerConfig cfg;
        UA_UsernamePasswordLogin login;
        memset(&cfg, 0, sizeof(cfg));
        cfg.logging = o6q_logger();
        t0 = o6q_now_us();
        rc = UA_ServerConfig_setDefaultWithSecurityPolicies(&cfg, (UA_UInt16)port,
                                                            &server_id.cert, &server_id.key,
                                                            &client_id.cert, 1, NULL, 0, NULL, 0);
        if(rc == UA_STATUSCODE_GOOD) {
            /* Anonymous and one user. (open62541 warns that every user has
             * all permissions; that is what this test wants.) */
            login.username = UA_STRING(USER);
            login.password = UA_STRING(PASSWORD);
            rc = UA_AccessControl_default(&cfg, true, NULL, 1, &login);
        }
        if(rc == UA_STATUSCODE_GOOD) {
            o6q_server_limits(&cfg);
            srv.server = UA_Server_newWithConfig(&cfg);
            if(!srv.server)
                rc = UA_STATUSCODE_BADINTERNALERROR;
        }
        if(rc == UA_STATUSCODE_GOOD)
            rc = o6q_model_add(srv.server, &model);
        if(rc == UA_STATUSCODE_GOOD)
            rc = o6q_server_run(&srv);
        CHECK("startup", rc == UA_STATUSCODE_GOOD,
              "server with certificate, trust list and 3 security policies -> %s in %.0f ms",
              SC(rc), (o6q_now_us() - t0) / 1000.0);
        if(rc != UA_STATUSCODE_GOOD)
            goto done;
    }

    /* ---- what the server offers ---- */
    o6q_step("endpoints");
    {
        UA_Client *c = o6q_client_new();
        UA_EndpointDescription *eps = NULL;
        size_t eps_n = 0;
        int secure = 0, none = 0, with_cert = 0;
        rc = c ? UA_Client_getEndpoints(c, url, &eps_n, &eps) : UA_STATUSCODE_BADOUTOFMEMORY;
        for(size_t i = 0; i < eps_n; i++) {
            if(eps[i].securityMode == UA_MESSAGESECURITYMODE_NONE)
                none++;
            else
                secure++;
            if(UA_ByteString_equal(&eps[i].serverCertificate, &server_id.cert))
                with_cert++;
        }
        CHECK("endpoints", rc == UA_STATUSCODE_GOOD && secure == 6 && none == 1,
              "GetEndpoints -> %s: %d secure endpoints (3 policies x Sign, SignAndEncrypt), %d "
              "without security, %d carry the server certificate",
              SC(rc), secure, none, with_cert);
        UA_Array_delete(eps, eps_n, &UA_TYPES[UA_TYPES_ENDPOINTDESCRIPTION]);
        if(c)
            UA_Client_delete(c);
    }

    /* ---- every policy in both modes ---- */
    for(size_t p = 0; p < POLICIES; p++) {
        test_policy(url, &client_id, &server_id, &policies[p], UA_MESSAGESECURITYMODE_SIGN,
                    reads);
        test_policy(url, &client_id, &server_id, &policies[p],
                    UA_MESSAGESECURITYMODE_SIGNANDENCRYPT, reads);
    }

    /* ---- user name and password on an encrypted channel ---- */
    o6q_step("user.password");
    {
        UA_Client *c = secure_client(&client_id, &server_id.cert, 1,
                                     UA_MESSAGESECURITYMODE_SIGNANDENCRYPT, policies[0].uri,
                                     CLIENT_URI);
        UA_Int32 v = 0;
        rc = c ? UA_Client_connectUsername(c, url, USER, PASSWORD) : UA_STATUSCODE_BADOUTOFMEMORY;
        if(rc == UA_STATUSCODE_GOOD)
            rc = read_answer(c, &v);
        CHECK("user.password", rc == UA_STATUSCODE_GOOD && v == 42,
              "user \"%s\" with the right password -> %s, answer=%d", USER, SC(rc), (int)v);
        drop(c);
    }

    o6q_step("user.wrongpassword");
    {
        UA_Client *c = secure_client(&client_id, &server_id.cert, 1,
                                     UA_MESSAGESECURITYMODE_SIGNANDENCRYPT, policies[0].uri,
                                     CLIENT_URI);
        o6q_expect_errors(1);
        rc = c ? UA_Client_connectUsername(c, url, USER, "not-the-password")
               : UA_STATUSCODE_BADOUTOFMEMORY;
        o6q_expect_errors(0);
        CHECK("user.wrongpassword", rc == UA_STATUSCODE_BADUSERACCESSDENIED,
              "user \"%s\" with a wrong password -> %s (expected BadUserAccessDenied)", USER,
              SC(rc));
        o6q_expect_errors(1);
        drop(c);
        o6q_expect_errors(0);
    }

    /* ---- connections that must be refused ---- */
    o6q_step("reject.untrusted_client");
    {
        /* The certificate of this client is not in the server's trust list. */
        UA_Client *c = secure_client(&rogue_id, &server_id.cert, 1,
                                     UA_MESSAGESECURITYMODE_SIGNANDENCRYPT, policies[0].uri,
                                     ROGUE_URI);
        o6q_expect_errors(1);
        rc = c ? UA_Client_connect(c, url) : UA_STATUSCODE_GOOD;
        CHECK("reject.untrusted_client", c != NULL && rc != UA_STATUSCODE_GOOD,
              "client with an unknown certificate -> %s", SC(rc));
        drop(c);
        o6q_expect_errors(0);
    }

    o6q_step("reject.untrusted_server");
    {
        /* This client trusts only itself, not the server. */
        UA_Client *c = secure_client(&client_id, &client_id.cert, 1,
                                     UA_MESSAGESECURITYMODE_SIGNANDENCRYPT, policies[0].uri,
                                     CLIENT_URI);
        o6q_expect_errors(1);
        rc = c ? UA_Client_connect(c, url) : UA_STATUSCODE_GOOD;
        CHECK("reject.untrusted_server", c != NULL && rc != UA_STATUSCODE_GOOD,
              "client that does not trust the server certificate -> %s", SC(rc));
        drop(c);
        o6q_expect_errors(0);
    }

    o6q_step("reject.wrong_uri");
    {
        /* Trusted certificate, but the application claims another identity
         * than the one written in it. */
        UA_Client *c = secure_client(&client_id, &server_id.cert, 1,
                                     UA_MESSAGESECURITYMODE_SIGNANDENCRYPT, policies[0].uri,
                                     "urn:o6q:somebody:else");
        o6q_expect_errors(1);
        rc = c ? UA_Client_connect(c, url) : UA_STATUSCODE_GOOD;
        CHECK("reject.wrong_uri", c != NULL && rc == UA_STATUSCODE_BADCERTIFICATEURIINVALID,
              "application URI that is not in the certificate -> %s (expected "
              "BadCertificateUriInvalid)",
              SC(rc));
        drop(c);
        o6q_expect_errors(0);
    }

    /* ---- and the server still serves the clients it should ---- */
    o6q_step("after_rejects");
    {
        UA_Client *c1 = secure_client(&client_id, &server_id.cert, 1,
                                      UA_MESSAGESECURITYMODE_SIGNANDENCRYPT, policies[2].uri,
                                      CLIENT_URI);
        UA_Client *c2 = o6q_client_new();
        UA_Int32 v1 = 0, v2 = 0;
        UA_StatusCode ra = c1 ? UA_Client_connect(c1, url) : UA_STATUSCODE_BADOUTOFMEMORY;
        UA_StatusCode rb = c2 ? UA_Client_connect(c2, url) : UA_STATUSCODE_BADOUTOFMEMORY;
        if(ra == UA_STATUSCODE_GOOD)
            ra = read_answer(c1, &v1);
        if(rb == UA_STATUSCODE_GOOD)
            rb = read_answer(c2, &v2);
        CHECK("after_rejects",
              ra == UA_STATUSCODE_GOOD && v1 == 42 && rb == UA_STATUSCODE_GOOD && v2 == 42,
              "trusted encrypted client %s, anonymous client without security %s", SC(ra),
              SC(rb));
        drop(c1);
        drop(c2);
    }

    /* ---- the trust list as a directory ---- */
#ifdef O6Q_HAVE_CERTFOLDERS
    test_certfolders(port + 1, &server_id, &client_id, &rogue_id);
#else
    o6q_skip("certfolder.trusted",
             "UA_CertificateVerification_CertFolders() of the release is for Linux only");
#endif

done:
    o6q_step("shutdown");
    o6q_server_stop(&srv);
    clear_identity(&server_id);
    clear_identity(&client_id);
    clear_identity(&rogue_id);
    return o6q_end();
}
