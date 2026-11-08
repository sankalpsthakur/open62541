/* Fork-only validation driver for issue 7675, not part of the PR.
 * For each seed, parse with a memory limit of 0, 1, 2, ... bytes until the
 * parse succeeds. So every allocation failure that a memory limit can cause
 * runs once, under ASan and LSan. */

#include "custom_memory_manager.h"

#include <open62541/types.h>
#include <open62541/util.h>

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char *seeds[] = {
    "SELECT /Message#Value, /Severity",
    "SELECT /Message, /Severity, /EventType WHERE OFTYPE ns=1;i=5001",
    "SELECT /Message, /Severity, /EventType WHERE /Severity >= 1000",
    "SELECT /Message, /Severity, /EventType WHERE $test FOR $test := /Severity >= $value, $value := 1000",
    "SELECT /Severity WHERE true AND false AND NOT true OR 123 BETWEEN [1,2]",
    "SELECT /Severity WHERE ((true AND false) AND (NOT true)) OR (123 BETWEEN [1,2])",
    "SELECT /Severity WHERE $test1 FOR $test1 := $test2, $test2 := OFTYPE ns=1;i=5001",
    "SELECT $select1 FOR $select1 := /Severity",
    "SELECT /Severity WHERE /Value == {\"UaType\": 3,\"Value\": [1,2,1,5],\"Dimension\": [2,2]}",
    "// Comment\nSELECT /Severity",
    "SELECT /Severity WHERE OFTYPE ns=1;s=DrillEventType AND (/Severity > 100 AND (NOT /1:Diameter BETWEEN [5,6] AND 75 >= /1:FeedForce)) OR /1:Result == \"Failed\"",
    "SELECT /Message#Value WHERE /Severity INLIST [1,2,3]",
    "SELECT /Severity WHERE /Severity == UINT16 7 OR /Severity == DOUBLE 1.5 OR /Message == STRING \"x\"",
    "SELECT /Severity WHERE /A == NODEID ns=1;i=1 AND /B == EXPANDEDNODEID nsu=http://x;i=1",
    "SELECT /Severity WHERE ISNULL /A OR /B LIKE 'a%' OR /C -> ns=0;i=6",
    "SELECT /Severity WHERE /A == b=AAEC"
};

static UA_StatusCode
parseWithLimit(const char *seed, unsigned long long limit) {
    UA_ByteString content;
    content.length = strlen(seed);
    content.data = (UA_Byte*)(uintptr_t)seed;
    UA_memoryManager_setLimit(limit);
    UA_EventFilter filter;
    UA_EventFilter_init(&filter);
    UA_StatusCode res = UA_EventFilter_parse(&filter, content, NULL);
    UA_EventFilter_clear(&filter);
    UA_memoryManager_setLimit(ULLONG_MAX);
    return res;
}

extern "C" int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    (void)data;
    (void)size;
    static bool done = false;
    if(done)
        return 0;
    done = true;
    size_t parsing = 0;
    for(size_t s = 0; s < sizeof(seeds) / sizeof(seeds[0]); s++) {
        UA_StatusCode full = parseWithLimit(seeds[s], ULLONG_MAX);
        unsigned long long cap = (full == UA_STATUSCODE_GOOD) ? (1ull << 22) : 65536;
        unsigned long long limit = 0;
        for(; limit < cap; limit++) {
            if(parseWithLimit(seeds[s], limit) == UA_STATUSCODE_GOOD)
                break;
        }
        if(full == UA_STATUSCODE_GOOD) {
            parsing++;
            printf("seed %zu parses with a limit of %llu bytes\n", s, limit);
            if(limit == cap)
                abort();
        } else {
            printf("seed %zu does not parse (%s), swept to %llu bytes\n",
                   s, UA_StatusCode_name(full), cap);
        }
    }
    printf("%zu of %zu seeds parse\n", parsing, sizeof(seeds) / sizeof(seeds[0]));
    return 0;
}
