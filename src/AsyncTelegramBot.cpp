#include "AsyncTelegramBot.h"

#if DEBUG_ENABLE
#define debugJson(X, Y)            \
    {                              \
        log_debug();               \
        Serial.println();          \
        serializeJsonPretty(X, Y); \
        Serial.println();          \
    }
#define errorJson(E)       \
    {                      \
        log_error();       \
        Serial.println();  \
        Serial.println(E); \
    }
#else
#define debugJson(X, Y)
#define errorJson(E)
#endif

#define HEADERS_END "\r\n\r\n"

AsyncTelegramBot::AsyncTelegramBot(Client &client)
{
    m_botusername.reserve(32); // Telegram username is 5-32 chars lenght
    m_rxbuffer.reserve(BUFFER_BIG);
    this->telegramClient = &client;
    m_minUpdateTime = MIN_UPDATE_TIME;
}

AsyncTelegramBot::~AsyncTelegramBot(){};

bool AsyncTelegramBot::checkConnection()
{
#if DEBUG_ENABLE
    static uint32_t lastCTime;
#endif
    // Start connection with Telegramn server (if necessary)
    if (!telegramClient->connected())
    {
        telegramClient->flush();
        telegramClient->clearWriteError();
        telegramClient->stop();
        telegramClient->stop();
        m_lastmsg_timestamp = millis();
        log_debug("Start handshaking...");
        if (!telegramClient->connect(TELEGRAM_HOST, TELEGRAM_PORT))
        {
            Serial.printf("\n\nUnable to connect to Telegram server\n");
        }
#if DEBUG_ENABLE
        else
        {
            log_debug("Connected using Telegram hostname\n"
                      "Last connection was %d seconds ago\n",
                      (int)(millis() - lastCTime) / 1000);
            lastCTime = millis();
        }
#endif
    }
    return telegramClient->connected();
}

bool AsyncTelegramBot::begin()
{
    checkConnection();
    return getMe();
}

bool AsyncTelegramBot::reset(void)
{
    log_debug("Restart Telegram connection\n");
    telegramClient->stop();
    m_lastmsg_timestamp = millis();
    m_waitingReply = false;
    return checkConnection();
}

bool AsyncTelegramBot::sendCommand(const char *const &command, const char *payload, bool blocking)
{
    if (checkConnection())
    {
        String httpBuffer((char *)0);
        httpBuffer.reserve(BUFFER_BIG);
        httpBuffer = "POST https://" TELEGRAM_HOST "/bot";
        httpBuffer += m_token;
        httpBuffer += "/";
        httpBuffer += command;
        // Let's use 1.0 protocol in order to avoid chunked transfer encoding
        httpBuffer += " HTTP/1.0"
                      "\nHost: api.telegram.org"
                      "\nConnection: keep-alive"
                      "\nContent-Type: application/json";
        httpBuffer += "\nContent-Length: ";
        httpBuffer += strlen(payload);
        httpBuffer += "\n\n";
        httpBuffer += payload;
        // Send the whole request in one go is much faster
        telegramClient->print(httpBuffer);
        //Serial.println(httpBuffer);

        m_waitingReply = true;
        // Blocking mode
        if (blocking)
        {
            if (!telegramClient->find((char *)HEADERS_END))
            {
                log_error("Invalid HTTP response");
                telegramClient->stop();
                return false;
            }
            // If there are incoming bytes available from the server, read them and print them:
            m_rxbuffer = "";
            while (telegramClient->available())
            {
                yield();
                m_rxbuffer += (char)telegramClient->read();
            }
            m_waitingReply = false;
            if (m_rxbuffer.indexOf("ok") > -1)
                return true;
        }
    }
    return false;
}

bool AsyncTelegramBot::getUpdates()
{
    // No response from Telegram server for a long time
    if (millis() - m_lastmsg_timestamp > 10 * m_minUpdateTime)
    {
        reset();
    }

    // Send message to Telegram server only if enough time has passed since last
    if (millis() - m_lastUpdateTime > m_minUpdateTime)
    {
        m_lastUpdateTime = millis();

        // If previuos reply from server was received (and parsed)
        if (m_waitingReply == false)
        {
            char payload[BUFFER_SMALL];
            snprintf(payload, BUFFER_SMALL, "{\"limit\":1,\"timeout\":0,\"offset\":%ld}", m_lastUpdateId);
            sendCommand("getUpdates", payload);
        }
    }

    if (telegramClient->connected() && telegramClient->available())
    {
        // We have a message, parse data received
        bool close_connection = false;

        // Skip headers
        while (telegramClient->connected())
        {
            String line = telegramClient->readStringUntil('\n');
            if (line == "\r")
                break;
            if (line.indexOf("close") > -1)
                close_connection = true;
        }

        // If there are incoming bytes available from the server, read them and store:
        m_rxbuffer = "";
        while (telegramClient->available())
        {
            yield();
            m_rxbuffer += (char)telegramClient->read();
        }
        m_waitingReply = false;
        m_lastmsg_timestamp = millis();

        if (close_connection)
        {
            telegramClient->stop();
            log_debug("Connection closed from server");
        }

        if (m_rxbuffer.indexOf("ok") < 0)
        {
            log_error("%s", m_rxbuffer.c_str());
            return false;
        }
        return true;
    }
    return false;
}

// Parse message received from Telegram server
MessageType AsyncTelegramBot::getNewMessage(TBMessage &message)
{
    message.messageType = MessageNoData;

    // We have a message, parse data received
    if (getUpdates())
    {
        DynamicJsonDocument updateDoc(BUFFER_BIG);
        deserializeJson(updateDoc, m_rxbuffer);
        m_rxbuffer = "";

        if (!updateDoc.containsKey("result"))
        {
            log_error("deserializeJson() failed with code");
            serializeJsonPretty(updateDoc, Serial);
            return MessageNoData;
        }

        uint32_t updateID = updateDoc["result"][0]["update_id"];
        if (!updateID)
            return MessageNoData;

        m_lastUpdateId = updateID + 1;
        debugJson(updateDoc, Serial);

        if (updateDoc["result"][0]["callback_query"]["id"])
        {
            // this is a callback query
            message.chatId = updateDoc["result"][0]["callback_query"]["message"]["chat"]["id"];
            message.sender.id = updateDoc["result"][0]["callback_query"]["from"]["id"];
            message.sender.username = updateDoc["result"][0]["callback_query"]["from"]["username"];
            message.sender.firstName = updateDoc["result"][0]["callback_query"]["from"]["first_name"];
            message.sender.lastName = updateDoc["result"][0]["callback_query"]["from"]["last_name"];
            message.messageID = updateDoc["result"][0]["callback_query"]["message"]["message_id"];
            message.date = updateDoc["result"][0]["callback_query"]["message"]["date"];
            message.chatInstance = updateDoc["result"][0]["callback_query"]["chat_instance"];
            message.callbackQueryID = updateDoc["result"][0]["callback_query"]["id"];
            message.callbackQueryData = updateDoc["result"][0]["callback_query"]["data"];
            message.text = updateDoc["result"][0]["callback_query"]["message"]["text"].as<String>();
            message.messageType = MessageQuery;

            // Check if callback function is defined for this button query
            for (uint8_t i = 0; i < m_keyboardCount; i++)
                m_keyboards[i]->checkCallback(message);
        }
        else if (updateDoc["result"][0]["message"]["message_id"])
        {
            // this is a message
            message.messageID = updateDoc["result"][0]["message"]["message_id"];
            message.chatId = updateDoc["result"][0]["message"]["chat"]["id"];
            message.sender.id = updateDoc["result"][0]["message"]["from"]["id"];
            message.sender.username = updateDoc["result"][0]["message"]["from"]["username"];
            message.sender.firstName = updateDoc["result"][0]["message"]["from"]["first_name"];
            message.sender.lastName = updateDoc["result"][0]["message"]["from"]["last_name"];
            message.sender.languageCode = updateDoc["result"][0]["message"]["from"]["language_code"];
            message.group.id = updateDoc["result"][0]["message"]["chat"]["id"];
            message.group.title = updateDoc["result"][0]["message"]["chat"]["title"];
            message.date = updateDoc["result"][0]["message"]["date"];

            if (updateDoc["result"][0]["message"]["location"])
            {
                // this is a location message
                message.location.longitude = updateDoc["result"][0]["message"]["location"]["longitude"];
                message.location.latitude = updateDoc["result"][0]["message"]["location"]["latitude"];
                message.messageType = MessageLocation;
            }
            else if (updateDoc["result"][0]["message"]["contact"])
            {
                // this is a contact message
                message.contact.id = updateDoc["result"][0]["message"]["contact"]["user_id"];
                message.contact.firstName = updateDoc["result"][0]["message"]["contact"]["first_name"];
                message.contact.lastName = updateDoc["result"][0]["message"]["contact"]["last_name"];
                message.contact.phoneNumber = updateDoc["result"][0]["message"]["contact"]["phone_number"];
                message.contact.vCard = updateDoc["result"][0]["message"]["contact"]["vcard"];
                message.messageType = MessageContact;
            }
            else if (updateDoc["result"][0]["message"]["document"])
            {
                // this is a document message
                message.document.file_id = updateDoc["result"][0]["message"]["document"]["file_id"];
                message.document.file_name = updateDoc["result"][0]["message"]["document"]["file_name"];
                message.text = updateDoc["result"][0]["message"]["caption"].as<String>();
                message.document.file_exists = getFile(message.document);
                message.messageType = MessageDocument;
            }
            else if (updateDoc["result"][0]["message"]["reply_to_message"])
            {
                // this is a reply to message
                message.text = updateDoc["result"][0]["message"]["text"].as<String>();
                message.messageType = MessageReply;
            }
            else if (updateDoc["result"][0]["message"]["text"])
            {
                // this is a text message
                message.text = updateDoc["result"][0]["message"]["text"].as<String>();
                message.messageType = MessageText;
            }
        }
        return message.messageType;
    }
    return MessageNoData; // waiting for reply from server
}

// Blocking getMe function (we wait for a reply from Telegram server)
bool AsyncTelegramBot::getMe()
{
    // getMe has to be blocking (wait server reply)
    if (!sendCommand("getMe", "", true))
    {
        log_error("getMe error ");
        return false;
    }
    StaticJsonDocument<BUFFER_SMALL> smallDoc;
    deserializeJson(smallDoc, m_rxbuffer);
    debugJson(smallDoc, Serial);
    m_botusername = smallDoc["result"]["username"].as<String>();
    return true;
}

bool AsyncTelegramBot::getFile(TBDocument &doc)
{
    char cmd[BUFFER_SMALL];
    snprintf(cmd, BUFFER_SMALL, "getFile?file_id=%s", doc.file_id);

    // getFile has to be blocking (wait server reply
    if (!sendCommand(cmd, "", true))
    {
        log_error("getFile error");
        return false;
    }
    StaticJsonDocument<BUFFER_MEDIUM> fileDoc;
    deserializeJson(fileDoc, m_rxbuffer);
    debugJson(fileDoc, Serial);
    doc.file_path = "https://api.telegram.org/file/bot";
    doc.file_path += m_token;
    doc.file_path += "/";
    doc.file_path += fileDoc["result"]["file_path"].as<String>();
    doc.file_size = fileDoc["result"]["file_size"].as<long>();
    return true;
}

bool AsyncTelegramBot::noNewMessage()
{

    TBMessage msg;
    this->reset();
    this->getNewMessage(msg);
    while (!this->getUpdates())
    {
        delay(100);
        // if(millis() - startTime > 10000UL)
        //     break;
    }
    log_debug("\n");
    return true;
}

bool AsyncTelegramBot::sendMessage(const TBMessage &msg, const char *message, const char *keyboard)
{
    if (!strlen(message))
        return false;

    DynamicJsonDocument root(BUFFER_BIG);
    // Backward compatibility
    root["chat_id"] = msg.sender.id != 0 ? msg.sender.id : msg.chatId;
    root["text"] = message;

    if (msg.isMarkdownEnabled)
        root["parse_mode"] = "MarkdownV2";
    else if (msg.isHTMLenabled)
        root["parse_mode"] = "HTML";

    if (msg.disable_notification)
        root["disable_notification"] = true;
    if (keyboard != nullptr)
    {
        if (strlen(keyboard) || msg.force_reply)
        {
            StaticJsonDocument<BUFFER_MEDIUM> doc;
            deserializeJson(doc, keyboard);
            JsonObject myKeyb = doc.as<JsonObject>();
            root["reply_markup"] = myKeyb;
            if (msg.force_reply)
            {
                root["reply_markup"]["selective"] = true,
                root["reply_markup"]["force_reply"] = true;
            }
        }
    }
    root.shrinkToFit();

    size_t len = measureJson(root);
    char payload[len];
    serializeJson(root, payload, len);

    debugJson(root, Serial);
    const bool result = sendCommand("sendMessage", payload);
    return result;
}

bool AsyncTelegramBot::sendTextMessage(int64_t chat_id, String text, String parse_mode = "Default", String entities = "", bool disable_web_page_preview = false, bool disable_notification = false, int32_t reply_to_message_id = 0, bool force_reply = false, bool allow_sending_without_reply = true, String reply_markup = "")
{
    if (!strlen(text.c_str()))
        return false;

    if (chat_id == 0)
        return false;

    DynamicJsonDocument root(BUFFER_BIG);
    // Backward compatibility
    root["chat_id"] = chat_id;
    root["text"] = text;

    if (parse_mode != "Default")
        if (parse_mode == "Markdown" || parse_mode == "MarkdownV2" || parse_mode == "HTML")
            root["parse_mode"] = parse_mode;

    if (disable_web_page_preview)
        root["disable_web_page_preview"] = disable_web_page_preview;

    if (disable_notification)
        root["disable_notification"] = disable_notification;

    if (reply_to_message_id != 0)
        root["reply_to_message_id"] = reply_to_message_id;

    if (allow_sending_without_reply)
        root["allow_sending_without_reply"] = allow_sending_without_reply;

    if (strlen(entities.c_str()))
    {
        StaticJsonDocument<BUFFER_MEDIUM> doc;
        deserializeJson(doc, entities);
        JsonObject myEntities = doc.as<JsonObject>();
        root["entities"] = myEntities;
    }

    if (strlen(reply_markup.c_str()) || force_reply)
    {
        StaticJsonDocument<BUFFER_MEDIUM> doc;
        deserializeJson(doc, reply_markup);
        JsonObject myKeyb = doc.as<JsonObject>();
        root["reply_markup"] = myKeyb;
        if (force_reply)
        {
            root["reply_markup"]["selective"] = true,
            root["reply_markup"]["force_reply"] = true;
        }
    }

    root.shrinkToFit();

    size_t len = measureJson(root);
    char payload[len];
    serializeJson(root, payload, len);

    debugJson(root, Serial);
    const bool result = sendCommand("sendMessage", payload);
    return result;
}

bool AsyncTelegramBot::forwardMessage(const TBMessage &msg, const int32_t to_chatid)
{
    char payload[BUFFER_SMALL];
    snprintf(payload, BUFFER_SMALL,
             "{\"chat_id\":%ld,\"from_chat_id\":%lld,\"message_id\":%ld}",
             to_chatid, msg.chatId, msg.messageID);

    const bool result = sendCommand("forwardMessage", payload);
    log_debug("%s", payload);
    return result;
}

bool AsyncTelegramBot::sendPhotoByUrl(const int64_t &chat_id, const char *url, const char *caption)
{
    if (!strlen(url))
        return false;

    char payload[BUFFER_SMALL];
    snprintf(payload, BUFFER_SMALL,
             "{\"chat_id\":%lld,\"photo\":\"%s\",\"caption\":\"%s\"}",
             chat_id, url, caption);

    const bool result = sendCommand("sendPhoto", payload);
    log_debug("%s", payload);
    return result;
}

bool AsyncTelegramBot::sendToChannel(const char *channel, const char *message, bool silent)
{
    if (!strlen(message))
        return false;

    char payload[BUFFER_MEDIUM];
    snprintf(payload, BUFFER_MEDIUM,
             "{\"chat_id\":\"%s\",\"text\":\"%s\",\"silent\":%s}",
             channel, message, silent ? "true" : "false");

    const bool result = sendCommand("sendMessage", payload);
    log_debug("%s", payload);
    return result;
}

bool AsyncTelegramBot::endQuery(const TBMessage &msg, const char *message, bool alertMode)
{
    if (!msg.callbackQueryID)
        return false;
    char payload[BUFFER_SMALL];
    snprintf(payload, BUFFER_SMALL,
             "{\"callback_query_id\":%s,\"text\":\"%s\",\"cache_time\":30,\"show_alert\":%s}",
             msg.callbackQueryID, message, alertMode ? "true" : "false");
    const bool result = sendCommand("answerCallbackQuery", payload, true);
    return result;
}

bool AsyncTelegramBot::removeReplyKeyboard(const TBMessage &msg, const char *message, bool selective)
{
    char payload[BUFFER_SMALL];
    snprintf(payload, BUFFER_SMALL,
             "{\"remove_keyboard\":true,\"selective\":%s}", selective ? "true" : "false");
    const bool result = sendMessage(msg, message, payload);
    return result;
}

char *int64_to_string(int64_t input)
{
    static char result[21] = "";
    // Clear result from any leftover digits from previous function call.
    memset(&result[0], 0, sizeof(result));
    // temp is used as a temporary result storage to prevent sprintf bugs.
    char temp[22] = "";
    char c;
    uint8_t base = 10;

    while (input)
    {
        int num = input % base;
        input /= base;
        c = '0' + num;
        snprintf(temp, sizeof(temp), "%c%s", c, result);
        strcpy(result, temp);
    }
    return result;
}

void AsyncTelegramBot::setformData(int64_t chat_id, const char *cmd, const char *type,
                                   const char *propName, size_t size, String &formData, String &request)
{

#define BOUNDARY "----WebKitFormBoundary7MA4YWxkTrZu0gW"
#define END_BOUNDARY "\r\n--" BOUNDARY "--\r\n"

    formData = "--" BOUNDARY "\r\nContent-disposition: form-data; name=\"chat_id\"\r\n\r\n";
    formData += int64_to_string(chat_id);
    formData += "\r\n--" BOUNDARY "\r\nContent-disposition: form-data; name=\"";
    formData += propName;
    formData += "\"; filename=\"image.jpg\"\r\nContent-Type: ";
    formData += type;
    formData += "\r\ncaption: \"image.jpg\"";
    formData += "\r\n\r\n";
    int contentLength = size + formData.length() + strlen(END_BOUNDARY);

    request = "POST /bot";
    request += m_token;
    request += "/";
    request += cmd;
    request += " HTTP/1.0\r\nHost: " TELEGRAM_HOST "\r\nContent-Length: ";
    request += contentLength;
    request += "\r\nContent-Type: multipart/form-data; boundary=" BOUNDARY "\r\n";
}

bool AsyncTelegramBot::sendStream(int64_t chat_id, const char *cmd, const char *type, const char *propName, Stream &stream, size_t size)
{
    bool res = false;
    if (checkConnection())
    {
        m_waitingReply = true;
        String formData;
        formData.reserve(512);
        String request;
        request.reserve(256);
        setformData(chat_id, cmd, type, propName, size, formData, request);

#if DEBUG_ENABLE
        uint32_t t1 = millis();
#endif
        // Send POST request to host
        telegramClient->println(request);
        // Body of request
        telegramClient->print(formData);

        uint8_t data[BLOCK_SIZE];
        int n_block = trunc(size / BLOCK_SIZE);
        int lastBytes = size - (n_block * BLOCK_SIZE);

        for (uint16_t pos = 0; pos < n_block; pos++)
        {
            stream.readBytes(data, BLOCK_SIZE);
            telegramClient->write(data, BLOCK_SIZE);
            yield();
        }
        stream.readBytes(data, lastBytes);
        telegramClient->write(data, lastBytes);

        // Close the request form-data
        telegramClient->println(END_BOUNDARY);
        telegramClient->flush();

#if DEBUG_ENABLE
        log_debug("Raw upload time: %lums\n", millis() - t1);
        t1 = millis();
#endif

        // Read server reply
        while (telegramClient->connected())
        {
            if (telegramClient->find((char *)"{\"ok\":true"))
            {
                res = true;
                break;
            }
        }
        log_debug("Read reply time: %lums\n", millis() - t1);
        telegramClient->stop();
        m_lastmsg_timestamp = millis();
        m_waitingReply = false;
        return res;
    }
    Serial.println("\nError: client not connected");
    return res;
}

bool AsyncTelegramBot::sendBuffer(int64_t chat_id, const char *cmd, const char *type, const char *propName, uint8_t *data, size_t size)
{
    bool res = false;
    if (checkConnection())
    {
        m_waitingReply = true;
        String formData;
        formData.reserve(512);
        String request;
        request.reserve(256);
        setformData(chat_id, cmd, type, propName, size, formData, request);

#if DEBUG_ENABLE
        uint32_t t1 = millis();
#endif
        // Send POST request to host
        telegramClient->println(request);
        // Body of request
        telegramClient->print(formData);

        // Serial.println(telegramClient->write((const uint8_t *) data, size));
        uint16_t pos = 0;
        int n_block = trunc(size / BLOCK_SIZE);
        int lastBytes = size - (n_block * BLOCK_SIZE);

        for (pos = 0; pos < n_block; pos++)
        {
            telegramClient->write((const uint8_t *)data + pos * BLOCK_SIZE, BLOCK_SIZE);
            yield();
        }
        telegramClient->write((const uint8_t *)data + pos * BLOCK_SIZE, lastBytes);

        // Close the request form-data
        telegramClient->println(END_BOUNDARY);
        telegramClient->flush();

#if DEBUG_ENABLE
        log_debug("Raw upload time: %lums\n", millis() - t1);
        t1 = millis();
#endif

        // Read server reply
        while (telegramClient->connected())
        {
            if (telegramClient->find((char *)"{\"ok\":true"))
            {
                res = true;
                break;
            }
        }
        log_debug("Read reply time: %lums\n", millis() - t1);
        telegramClient->stop();
        m_lastmsg_timestamp = millis();
        m_waitingReply = false;
        return res;
    }

    Serial.println("\nError: client not connected");
    return res;
}

void AsyncTelegramBot::getMyCommands(String &cmdList)
{
    if (!sendCommand("getMyCommands", "", true))
    {
        log_error("getMyCommands error ");
        return;
    }
    StaticJsonDocument<BUFFER_MEDIUM> doc;
    DeserializationError err = deserializeJson(doc, m_rxbuffer);
    if (err)
    {
        return;
    }
    debugJson(doc, Serial);
    //cmdList = doc["result"].as<String>();
    serializeJsonPretty(doc["result"], cmdList);
}

bool AsyncTelegramBot::deleteMyCommands()
{
    if (!sendCommand("deleteMyCommands", "", true))
    {
        log_error("getMyCommands error ");
        return "";
    }
    return true;
}

bool AsyncTelegramBot::setMyCommands(const String &cmd, const String &desc)
{

    // get actual list of commands
    if (!sendCommand("getMyCommands", "", true))
    {
        log_error("getMyCommands error ");
        return "";
    }
    DynamicJsonDocument doc(BUFFER_MEDIUM);
    DeserializationError err = deserializeJson(doc, m_rxbuffer);
    if (err)
    {
        return false;
    }

    // Check if command already present in list
    for (JsonObject key : doc["result"].as<JsonArray>())
    {
        if (key["command"] == cmd)
        {
            return false;
        }
    }

    StaticJsonDocument<256> obj;
    obj["command"] = cmd;
    obj["description"] = desc;
    doc["result"].as<JsonArray>().add(obj);

    StaticJsonDocument<BUFFER_MEDIUM> doc2;
    doc2["commands"] = doc["result"].as<JsonArray>();

    size_t len = measureJson(doc2);
    char payload[len];
    serializeJson(doc2, payload, len);
    debugJson(doc2, Serial);

    const bool result = sendCommand("setMyCommands", payload, true);
    return result;
}

bool AsyncTelegramBot::editMessage(int32_t chat_id, int32_t message_id, const String &txt, const String &keyboard)
{
    String payload = "{\"chat_id\":";
    payload += chat_id;
    payload += ",\"message_id\":";
    payload += message_id;
    payload += ", \"text\": \"";
    payload += txt;

    if (keyboard.length())
    {
        payload += "\", \"reply_markup\": ";
        payload += keyboard;
        payload += "}";
    }
    else
    {
        payload += "\"}";
    }

    const bool result = sendCommand("editMessageText", payload.c_str());

    return result;
}