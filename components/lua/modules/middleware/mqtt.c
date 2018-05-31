/*
 * Copyright (C) 2015 - 2018, IBEROXARXA SERVICIOS INTEGRALES, S.L.
 * Copyright (C) 2015 - 2018, Jaume Olivé Petrus (jolive@whitecatboard.org)
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the <organization> nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *     * The WHITECAT logotype cannot be changed, you can remove it, but you
 *       cannot change it in any way. The WHITECAT logotype is:
 *
 *          /\       /\
 *         /  \_____/  \
 *        /_____________\
 *        W H I T E C A T
 *
 *     * Redistributions in binary form must retain all copyright notices printed
 *       to any local or remote output device. This include any reference to
 *       Lua RTOS, whitecatboard.org, Lua, and other copyright notices that may
 *       appear in the future.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Lua RTOS, Lua MQTT module
 *
 */

#include "luartos.h"

#if CONFIG_LUA_RTOS_LUA_USE_MQTT
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "modules.h"
#include "error.h"
#include "sys.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <mqtt/MQTTClient.h>
#include <mqtt/MQTTClientPersistence.h>

#include <sys/mutex.h>
#include <sys/delay.h>
#include <sys/status.h>
#include <sys/syslog.h>
#include <sys/mount.h>

#include <sys/drivers/net.h>

#define MQTT_CONNECT_TIMEOUT 20

void MQTTClient_init();

// Module errors
#define LUA_MQTT_ERR_CANT_CREATE_CLIENT (DRIVER_EXCEPTION_BASE(MQTT_DRIVER_ID) |  0)
#define LUA_MQTT_ERR_CANT_SET_CALLBACKS (DRIVER_EXCEPTION_BASE(MQTT_DRIVER_ID) |  1)
#define LUA_MQTT_ERR_CANT_CONNECT       (DRIVER_EXCEPTION_BASE(MQTT_DRIVER_ID) |  2)
#define LUA_MQTT_ERR_CANT_SUBSCRIBE     (DRIVER_EXCEPTION_BASE(MQTT_DRIVER_ID) |  3)
#define LUA_MQTT_ERR_CANT_PUBLISH       (DRIVER_EXCEPTION_BASE(MQTT_DRIVER_ID) |  4)
#define LUA_MQTT_ERR_CANT_DISCONNECT    (DRIVER_EXCEPTION_BASE(MQTT_DRIVER_ID) |  5)
#define LUA_MQTT_ERR_LOST_CONNECTION    (DRIVER_EXCEPTION_BASE(MQTT_DRIVER_ID) |  6)
#define LUA_MQTT_ERR_NOT_ENOUGH_MEMORY  (DRIVER_EXCEPTION_BASE(MQTT_DRIVER_ID) |  7)

// Register driver and messages
DRIVER_REGISTER_BEGIN(MQTT,mqtt,0,NULL,NULL);
    DRIVER_REGISTER_ERROR(MQTT, mqtt, CannotCreateClient, "can't create client", LUA_MQTT_ERR_CANT_CREATE_CLIENT);
    DRIVER_REGISTER_ERROR(MQTT, mqtt, CannotSetCallbacks, "can't set callbacks", LUA_MQTT_ERR_CANT_SET_CALLBACKS);
    DRIVER_REGISTER_ERROR(MQTT, mqtt, CannotConnect, "can't connect", LUA_MQTT_ERR_CANT_CONNECT);
    DRIVER_REGISTER_ERROR(MQTT, mqtt, CannotSubscribeToTopic, "can't subscribe to topic", LUA_MQTT_ERR_CANT_SUBSCRIBE);
    DRIVER_REGISTER_ERROR(MQTT, mqtt, CannotPublishToTopic, "can't publish to topic", LUA_MQTT_ERR_CANT_PUBLISH);
    DRIVER_REGISTER_ERROR(MQTT, mqtt, CannotDisconnect, "can't disconnect", LUA_MQTT_ERR_CANT_DISCONNECT);
    DRIVER_REGISTER_ERROR(MQTT, mqtt, LostConnection, "lost connection", LUA_MQTT_ERR_LOST_CONNECTION);
    DRIVER_REGISTER_ERROR(MQTT, mqtt, NotEnoughMemory, "not enough memory", LUA_MQTT_ERR_NOT_ENOUGH_MEMORY);
DRIVER_REGISTER_END(MQTT,mqtt,0,NULL,NULL);

static int client_inited = 0;

typedef struct {
    char *topic;              // Topic
    lua_callback_t *callback; // Lua callback
    void *next;              // Next callback
} mqtt_subs_callback;

typedef struct {
    struct mtx callback_mtx;

    MQTTClient_connectOptions conn_opts;
    MQTTClient_SSLOptions ssl_opts;
    MQTTClient client;

    mqtt_subs_callback *callbacks;
    const char *ca_file;

    int secure;
    int persistence;
} mqtt_userdata;

// Extracted from:
//
// http://git.eclipse.org/c/mosquitto/org.eclipse.mosquitto.git/tree/lib/util_mosq.c
//
// mosquitto_topic_matches_sub function
static int topic_matches_sub(const char *sub, const char *topic) {
    int slen, tlen;
    int spos, tpos;
    bool multilevel_wildcard = false;

    slen = strlen(sub);
    tlen = strlen(topic);

    if (slen && tlen) {
        if ((sub[0] == '$' && topic[0] != '$')
                || (topic[0] == '$' && sub[0] != '$')) {

            return 0;
        }
    }

    spos = 0;
    tpos = 0;

    while (spos < slen && tpos < tlen) {
        if (sub[spos] == topic[tpos]) {
            if (tpos == tlen - 1) {
                /* Check for e.g. foo matching foo/# */
                if (spos == slen - 3 && sub[spos + 1] == '/'
                        && sub[spos + 2] == '#') {
                    multilevel_wildcard = true;
                    return 1;
                }
            }
            spos++;
            tpos++;
            if (spos == slen && tpos == tlen) {
                return 1;
            } else if (tpos == tlen && spos == slen - 1 && sub[spos] == '+') {
                spos++;
                return 1;
            }
        } else {
            if (sub[spos] == '+') {
                spos++;
                while (tpos < tlen && topic[tpos] != '/') {
                    tpos++;
                }
                if (tpos == tlen && spos == slen) {
                    return 1;
                }
            } else if (sub[spos] == '#') {
                multilevel_wildcard = true;
                if (spos + 1 != slen) {
                    return 0;
                } else {
                    return 1;
                }
            } else {
                return 0;
            }
        }
    }

    if (multilevel_wildcard == false && (tpos < tlen || spos < slen)) {
        return 0;
    }

    return 0;
}

static int add_subs_callback(lua_State *L, int index, mqtt_userdata *mqtt, const char *topic) {
    // Create and populate callback structure
    mqtt_subs_callback *callback = (mqtt_subs_callback *)calloc(1, sizeof(mqtt_subs_callback));
    if (!callback) {
        return luaL_exception_extended(L, LUA_MQTT_ERR_NOT_ENOUGH_MEMORY, NULL);
    }
    callback->topic = strdup(topic);
    if (!callback->topic) {
        free(callback);
        return luaL_exception_extended(L, LUA_MQTT_ERR_NOT_ENOUGH_MEMORY, NULL);
    }

    callback->callback = luaS_callback_create(L, index);
    if (callback->callback == NULL) {
        free(callback->topic);
        free(callback);

        return luaL_exception_extended(L, LUA_MQTT_ERR_NOT_ENOUGH_MEMORY, NULL);
    }

    mtx_lock(&mqtt->callback_mtx);
    callback->next = mqtt->callbacks;
    mqtt->callbacks = callback;
    mtx_unlock(&mqtt->callback_mtx);

    return 0;
}

static int messageArrived(void *context, char * topicName, int topicLen, MQTTClient_message* m) {
    mqtt_userdata *mqtt = (mqtt_userdata *) context;
    if (mqtt) {

        mqtt_subs_callback *callback;

        mtx_lock(&mqtt->callback_mtx);

        callback = mqtt->callbacks;
        while (callback) {
            if (topic_matches_sub(callback->topic, topicName)) {
                // Push argument for the callback's function
                lua_pushinteger(luaS_callback_state(callback->callback), m->payloadlen);
                lua_pushlstring(luaS_callback_state(callback->callback), m->payload, m->payloadlen);

                // see: https://www.ibm.com/support/knowledgecenter/SSFKSJ_7.5.0/com.ibm.mq.javadoc.doc/WMQMQxrCClasses/_m_q_t_t_client_8h.html?view=kc#aa42130dd069e7e949bcab37b6dce64a5
                if (topicLen == 0) {
                    lua_pushinteger(luaS_callback_state(callback->callback), strlen(topicName));
                    lua_pushstring(luaS_callback_state(callback->callback), topicName);
                } else {
                    lua_pushinteger(luaS_callback_state(callback->callback), topicLen);
                    lua_pushlstring(luaS_callback_state(callback->callback), topicName, topicLen);
                }

                luaS_callback_call(callback->callback, 4);
            }
            callback = callback->next;
        }

        mtx_unlock(&mqtt->callback_mtx);

        MQTTClient_freeMessage(&m);
        MQTTClient_free(topicName);

    }
    return 1;
}

static void connectionLost(void* context, char* cause) {
    mqtt_userdata *mqtt = (mqtt_userdata *) context;

    if (!mqtt)
        return;

    int rc = 0;

    for (;;) {
        if (mqtt->callback_mtx.lock) {
            mtx_lock(&mqtt->callback_mtx);

            if (NETWORK_AVAILABLE()) {
                rc = MQTTClient_connect(mqtt->client, &mqtt->conn_opts);
                if (rc == 0) {
                    mtx_unlock(&mqtt->callback_mtx);
                    break;
                } else {
                    mtx_unlock(&mqtt->callback_mtx);
                }
            } else {
                mtx_unlock(&mqtt->callback_mtx);
                usleep(500 * 1000);
            }
        } else {
            break;
        }
    }
}

// Lua: result = setup( id, clock )
static int lmqtt_client(lua_State* L) {
    int rc = 0;
    size_t lenClientId, lenHost;
    mqtt_userdata *mqtt;
    char url[250];
    const char *persistence_folder = NULL;
    int persistence = MQTTCLIENT_PERSISTENCE_NONE;

    const char *clientId = luaL_checklstring(L, 1, &lenClientId); //is being strdup'd in MQTTClient_create
    const char *host = luaL_checklstring(L, 2, &lenHost); //url is being strdup'd in MQTTClient_create
    int port = luaL_checkinteger(L, 3);

    luaL_checktype(L, 4, LUA_TBOOLEAN);
    int secure = lua_toboolean(L, 4);

    const char *ca_file = luaL_optstring(L, 5, NULL); //is being strdup'd below

    if (lua_gettop(L) > 5) {
        luaL_checktype(L, 6, LUA_TBOOLEAN);
        persistence =
                lua_toboolean(L, 6) ?
                        MQTTCLIENT_PERSISTENCE_DEFAULT :
                        MQTTCLIENT_PERSISTENCE_NONE;
        persistence_folder = luaL_optstring(L, 7, NULL); //is being strdup'd in MQTTClient_create
    }

    // Allocate mqtt structure and initialize
    mqtt = (mqtt_userdata *) lua_newuserdata(L, sizeof(mqtt_userdata));
    mqtt->client = NULL;
    mqtt->callbacks = NULL;
    mqtt->secure = secure;
    mqtt->persistence = persistence;
    mqtt->ca_file = (ca_file ? strdup(ca_file) : NULL); //save for use during mqtt_connect
    mtx_init(&mqtt->callback_mtx, NULL, NULL, 0);

    // needed for lmqtt_client_gc to not crash freeing the username and password
    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    bcopy(&conn_opts, &mqtt->conn_opts, sizeof(MQTTClient_connectOptions));

    // Calculate uri
    snprintf(url, sizeof(url), "%s://%s:%d", mqtt->secure ? "ssl" : "tcp", host, port);

    if (!client_inited) {
        MQTTClient_init();
        client_inited = 1;
    }

    //url is being strdup'd in MQTTClient_create
    rc = MQTTClient_create(&mqtt->client, url, clientId, persistence,
            (char*) persistence_folder);
    if (rc < 0) {
        return luaL_exception(L, LUA_MQTT_ERR_CANT_CREATE_CLIENT);
    }

    rc = MQTTClient_setCallbacks(mqtt->client, mqtt, connectionLost,
            messageArrived, NULL);
    if (rc < 0) {
        return luaL_exception(L, LUA_MQTT_ERR_CANT_SET_CALLBACKS);
    }

    luaL_getmetatable(L, "mqtt.cli");
    lua_setmetatable(L, -2);

    return 1;
}

static int lmqtt_connected(lua_State* L) {
    int rc;
    mqtt_userdata *mqtt = NULL;

    mqtt = (mqtt_userdata *) luaL_checkudata(L, 1, "mqtt.cli");
    luaL_argcheck(L, mqtt, 1, "mqtt expected");

    rc = MQTTClient_connected(mqtt->client);

    lua_pushboolean(L, rc == MQTTCLIENT_SUCCESS ? 1 : 0);

    return 1;
}

static int lmqtt_connect(lua_State* L) {
    int rc;
    const char *user;
    const char *password;
    mqtt_userdata *mqtt = NULL;

    mqtt = (mqtt_userdata *) luaL_checkudata(L, 1, "mqtt.cli");
    luaL_argcheck(L, mqtt, 1, "mqtt expected");

    user = luaL_checkstring(L, 2);
    password = luaL_checkstring(L, 3);

    MQTTClient_SSLOptions ssl_opts = MQTTClient_SSLOptions_initializer;
    ssl_opts.trustStore = mqtt->ca_file; //has been strdup'd already
    ssl_opts.enableServerCertAuth = (ssl_opts.trustStore != NULL);
    bcopy(&ssl_opts, &mqtt->ssl_opts, sizeof(MQTTClient_SSLOptions));

    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    conn_opts.connectTimeout = MQTT_CONNECT_TIMEOUT;
    conn_opts.keepAliveInterval = 60;
    conn_opts.reliable = 0;
    conn_opts.cleansession = 0;
    conn_opts.username = strdup(user); //needs to be strdup'd here for connectionLost usage
    conn_opts.password = strdup(password); // //needs to be strdup'd here for connectionLost usage
    conn_opts.ssl = &mqtt->ssl_opts;
    bcopy(&conn_opts, &mqtt->conn_opts, sizeof(MQTTClient_connectOptions));

    wait_for_network(MQTT_CONNECT_TIMEOUT);

    rc = MQTTClient_connect(mqtt->client, &mqtt->conn_opts);
    if (rc < 0) {
        return luaL_exception(L, LUA_MQTT_ERR_CANT_CONNECT);
    }

    return 0;
}

static int lmqtt_subscribe(lua_State* L) {
    int rc;
    int qos;
    const char *topic;

    mqtt_userdata *mqtt = NULL;

    mqtt = (mqtt_userdata *) luaL_checkudata(L, 1, "mqtt.cli");
    luaL_argcheck(L, mqtt, 1, "mqtt expected");

    topic = luaL_checkstring(L, 2);
    qos = luaL_checkinteger(L, 3);

    if (qos > 0 && mqtt->persistence == MQTTCLIENT_PERSISTENCE_NONE) {
        return luaL_exception_extended(L, LUA_MQTT_ERR_CANT_SUBSCRIBE,
                "enable persistence for a qos > 0");
    }

    // Add callback
    add_subs_callback(L, 4, mqtt, topic);

    wait_for_network(MQTT_CONNECT_TIMEOUT);

    rc = MQTTClient_subscribe(mqtt->client, topic, qos);
    if (rc == 0) {
        return 0;
    } else {
        return luaL_exception(L, LUA_MQTT_ERR_CANT_SUBSCRIBE);
    }
}

static int lmqtt_publish(lua_State* L) {
    int rc;
    int qos;
    size_t payload_len;
    const char *topic;
    char *payload;

    mqtt_userdata *mqtt = NULL;

    mqtt = (mqtt_userdata *) luaL_checkudata(L, 1, "mqtt.cli");
    luaL_argcheck(L, mqtt, 1, "mqtt expected");

    topic = luaL_checkstring(L, 2);
    payload = (char *) luaL_checklstring(L, 3, &payload_len);
    qos = luaL_checkinteger(L, 4);

    if (qos > 0 && mqtt->persistence == MQTTCLIENT_PERSISTENCE_NONE) {
        return luaL_exception_extended(L, LUA_MQTT_ERR_CANT_PUBLISH,
                "enable persistence for a qos > 0");
    }

    wait_for_network(MQTT_CONNECT_TIMEOUT);

    rc = MQTTClient_publish(mqtt->client, topic, payload_len, payload, qos, 0, NULL);

    if (rc == 0) {
        return 0;
    } else {
        return luaL_exception(L, LUA_MQTT_ERR_CANT_PUBLISH);
    }
}

static int lmqtt_disconnect(lua_State* L) {
    int rc = 0;

    mqtt_userdata *mqtt = NULL;

    mqtt = (mqtt_userdata *) luaL_checkudata(L, 1, "mqtt.cli");
    luaL_argcheck(L, mqtt, 1, "mqtt expected");

    if (MQTTClient_isConnected(mqtt->client)) {
        rc = MQTTClient_disconnect(mqtt->client, 0);
    }
    if (rc == 0) {
        return 0;
    } else {
        return luaL_exception(L, LUA_MQTT_ERR_CANT_DISCONNECT);
    }
}

// Destructor
static int lmqtt_client_gc(lua_State *L) {
    mqtt_userdata *mqtt = NULL;
    mqtt_subs_callback *callback;
    mqtt_subs_callback *nextcallback;

    mqtt = (mqtt_userdata *) luaL_testudata(L, 1, "mqtt.cli");
    if (mqtt && mqtt->callback_mtx.lock) {
        // Destroy callbacks
        mtx_lock(&mqtt->callback_mtx);

        callback = mqtt->callbacks;
        while (callback) {
            luaS_callback_destroy(callback->callback);

            nextcallback = callback->next;

            free(callback);
            callback = nextcallback;
        }
        mqtt->callbacks = NULL;

        mtx_unlock(&mqtt->callback_mtx);

        // Disconnect and destroy client
        if (MQTTClient_isConnected(mqtt->client)) {
            MQTTClient_disconnect(mqtt->client, 0);
        }
        MQTTClient_destroy(&mqtt->client);
        mqtt->client = NULL;

        mtx_destroy(&mqtt->callback_mtx);
        //mtx_destroy does mqtt->callback_mtx.lock = 0;

        if (mqtt->ca_file) {
            free((char*) mqtt->ca_file);
            mqtt->ca_file = NULL;
            mqtt->ssl_opts.trustStore = NULL;
        }
        if (mqtt->conn_opts.username) {
            free((char*) mqtt->conn_opts.username);
            mqtt->conn_opts.username = NULL;
        }
        if (mqtt->conn_opts.password) {
            free((char*) mqtt->conn_opts.password);
            mqtt->conn_opts.password = NULL;
        }
    }

    return 0;
}

static const LUA_REG_TYPE lmqtt_map[] = {
    { LSTRKEY( "client"      ),   LFUNCVAL( lmqtt_client     ) },

    { LSTRKEY("QOS0"), LINTVAL(0) },
    { LSTRKEY("QOS1"), LINTVAL(1) },
    { LSTRKEY("QOS2"), LINTVAL(2) },

    { LSTRKEY("PERSISTENCE_FILE"), LINTVAL(MQTTCLIENT_PERSISTENCE_DEFAULT) },
    { LSTRKEY("PERSISTENCE_NONE"), LINTVAL(MQTTCLIENT_PERSISTENCE_NONE) },
    { LSTRKEY("PERSISTENCE_USER"), LINTVAL(MQTTCLIENT_PERSISTENCE_USER) },

    // Error definitions
    DRIVER_REGISTER_LUA_ERRORS(mqtt)
    { LNILKEY, LNILVAL }
};

static const LUA_REG_TYPE lmqtt_client_map[] = {
    { LSTRKEY( "connect"     ),   LFUNCVAL( lmqtt_connect    ) },
    { LSTRKEY( "connected"   ),   LFUNCVAL( lmqtt_connected  ) },
    { LSTRKEY( "disconnect"  ),   LFUNCVAL( lmqtt_disconnect ) },
    { LSTRKEY( "subscribe"   ),   LFUNCVAL( lmqtt_subscribe  ) },
    { LSTRKEY( "publish"     ),   LFUNCVAL( lmqtt_publish    ) },
    { LSTRKEY( "__metatable" ),   LROVAL  ( lmqtt_client_map ) },
    { LSTRKEY( "__index"     ),   LROVAL  ( lmqtt_client_map ) },
    { LSTRKEY( "__gc"        ),   LFUNCVAL( lmqtt_client_gc  ) },
    { LNILKEY, LNILVAL }
};

LUALIB_API int luaopen_mqtt( lua_State *L ) {
    luaL_newmetarotable(L,"mqtt.cli", (void *)lmqtt_client_map);

#if !LUA_USE_ROTABLE
    luaL_newlib(L, mqtt);
    return 1;
#else
    return 0;
#endif
}

MODULE_REGISTER_ROM(MQTT, mqtt, lmqtt_map, luaopen_mqtt, 1);

#endif
