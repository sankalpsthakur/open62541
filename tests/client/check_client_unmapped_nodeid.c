/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Regression test for https://github.com/open62541/open62541/issues/8486:
 * reading a NodeId / ExpandedNodeId value whose NamespaceIndex is not in the
 * server's NamespaceArray must preserve that index instead of rewriting it to
 * UINT16_MAX - index during client-side namespace mapping. */

#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>
#include <open62541/server.h>
#include <open62541/server_config_default.h>

#include "test_helpers.h"
#include "thread_wrapper.h"

#include <check.h>
#include <stdlib.h>

static UA_Server *server;
static UA_atomic(uintptr_t) running;
static THREAD_HANDLE server_thread;

#define EXTERNAL_NS 10

THREAD_CALLBACK(serverloop) {
    while(UA_atomic_load(&running))
        UA_Server_run_iterate(server, true);
    return 0;
}

static UA_NodeId
nodeIdNode(void) {
    return UA_NODEID_STRING(1, (char *)"NodeIdNode");
}

static UA_NodeId
expandedNodeIdNode(void) {
    return UA_NODEID_STRING(1, (char *)"ExpandedNodeIdNode");
}

static void
addValueVariable(UA_NodeId nodeId, char *name, void *value,
                 const UA_DataType *type) {
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    UA_Variant_setScalar(&attr.value, value, type);
    attr.displayName = UA_LOCALIZEDTEXT("en-US", name);
    attr.description = UA_LOCALIZEDTEXT("en-US", name);
    attr.dataType = type->typeId;
    attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    UA_StatusCode ret = UA_Server_addVariableNode(
        server, nodeId,
        UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
        UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
        UA_QUALIFIEDNAME(1, name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
        attr, NULL, NULL);
    ck_assert_uint_eq(ret, UA_STATUSCODE_GOOD);
}

static void setup(void) {
    server = UA_Server_newForUnitTest();
    ck_assert_ptr_ne(server, NULL);

    /* NodeId whose namespace index is not declared in the server NamespaceArray */
    UA_NodeId nodeIdValue =
        UA_NODEID_STRING(EXTERNAL_NS, (char *)"MyIdInExternalNamespace");
    addValueVariable(nodeIdNode(), (char *)"NodeIdNode", &nodeIdValue,
                     &UA_TYPES[UA_TYPES_NODEID]);

    UA_ExpandedNodeId expandedValue =
        UA_EXPANDEDNODEID_STRING(EXTERNAL_NS, (char *)"MyIdInExternalNamespace");
    addValueVariable(expandedNodeIdNode(), (char *)"ExpandedNodeIdNode",
                     &expandedValue, &UA_TYPES[UA_TYPES_EXPANDEDNODEID]);

    UA_Server_run_startup(server);
    UA_atomic_store(&running, true);
    THREAD_CREATE(server_thread, serverloop);
}

static void teardown(void) {
    UA_atomic_store(&running, false);
    THREAD_JOIN(server_thread);
    UA_Server_run_shutdown(server);
    UA_Server_delete(server);
}

static UA_Client *
connectClient(void) {
    UA_Client *client = UA_Client_newForUnitTest();
    UA_StatusCode res = UA_Client_connect(client, "opc.tcp://localhost:4840");
    ck_assert_uint_eq(res, UA_STATUSCODE_GOOD);
    return client;
}

static void
disconnectClient(UA_Client *client) {
    UA_Client_disconnect(client);
    UA_Client_delete(client);
}

START_TEST(Client_readUnmappedNodeId) {
    UA_Client *client = connectClient();

    UA_Variant var;
    UA_Variant_init(&var);
    UA_StatusCode res = UA_Client_readValueAttribute(client, nodeIdNode(), &var);
    ck_assert_uint_eq(res, UA_STATUSCODE_GOOD);
    ck_assert(UA_Variant_isScalar(&var));
    ck_assert_ptr_eq(var.type, &UA_TYPES[UA_TYPES_NODEID]);

    const UA_NodeId *readId = (const UA_NodeId *)var.data;
    UA_String expectedId = UA_STRING("MyIdInExternalNamespace");
    ck_assert_uint_eq(readId->namespaceIndex, EXTERNAL_NS);
    ck_assert_uint_eq(readId->identifierType, UA_NODEIDTYPE_STRING);
    ck_assert(UA_String_equal(&readId->identifier.string, &expectedId));

    UA_Variant_clear(&var);
    disconnectClient(client);
} END_TEST

START_TEST(Client_readUnmappedExpandedNodeId) {
    UA_Client *client = connectClient();

    UA_Variant var;
    UA_Variant_init(&var);
    UA_StatusCode res =
        UA_Client_readValueAttribute(client, expandedNodeIdNode(), &var);
    ck_assert_uint_eq(res, UA_STATUSCODE_GOOD);
    ck_assert(UA_Variant_isScalar(&var));
    ck_assert_ptr_eq(var.type, &UA_TYPES[UA_TYPES_EXPANDEDNODEID]);

    const UA_ExpandedNodeId *readId = (const UA_ExpandedNodeId *)var.data;
    UA_String expectedId = UA_STRING("MyIdInExternalNamespace");
    ck_assert_uint_eq(readId->nodeId.namespaceIndex, EXTERNAL_NS);
    ck_assert_uint_eq(readId->nodeId.identifierType, UA_NODEIDTYPE_STRING);
    ck_assert(UA_String_equal(&readId->nodeId.identifier.string, &expectedId));

    UA_Variant_clear(&var);
    disconnectClient(client);
} END_TEST

START_TEST(Client_writeUnmappedNodeId) {
    /* Writing ns=10 used to store UINT16_MAX-10 on the server; the client then
     * mapped that sentinel back to 10 on read, hiding the corruption. Verify
     * the value that actually lands in the information model. */
    UA_Client *client = connectClient();

    UA_NodeId written = UA_NODEID_STRING(EXTERNAL_NS, (char *)"WrittenExternal");
    UA_Variant var;
    UA_Variant_init(&var);
    UA_Variant_setScalar(&var, &written, &UA_TYPES[UA_TYPES_NODEID]);
    UA_StatusCode res = UA_Client_writeValueAttribute(client, nodeIdNode(), &var);
    ck_assert_uint_eq(res, UA_STATUSCODE_GOOD);
    disconnectClient(client);

    UA_Variant serverVal;
    UA_Variant_init(&serverVal);
    res = UA_Server_readValue(server, nodeIdNode(), &serverVal);
    ck_assert_uint_eq(res, UA_STATUSCODE_GOOD);
    ck_assert(UA_Variant_isScalar(&serverVal));
    ck_assert_ptr_eq(serverVal.type, &UA_TYPES[UA_TYPES_NODEID]);

    const UA_NodeId *stored = (const UA_NodeId *)serverVal.data;
    UA_String expectedId = UA_STRING("WrittenExternal");
    ck_assert_uint_eq(stored->namespaceIndex, EXTERNAL_NS);
    ck_assert(UA_String_equal(&stored->identifier.string, &expectedId));

    UA_Variant_clear(&serverVal);
} END_TEST

static Suite *testSuite_unmappedNodeId(void) {
    TCase *tc = tcase_create("UnmappedNamespaceIndex");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, Client_readUnmappedNodeId);
    tcase_add_test(tc, Client_readUnmappedExpandedNodeId);
    tcase_add_test(tc, Client_writeUnmappedNodeId);

    Suite *s = suite_create("Client Unmapped NodeId");
    suite_add_tcase(s, tc);
    return s;
}

int main(void) {
    Suite *s = testSuite_unmappedNodeId();
    SRunner *sr = srunner_create(s);
    srunner_set_fork_status(sr, CK_NOFORK);
    srunner_run_all(sr, CK_NORMAL);
    int number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
