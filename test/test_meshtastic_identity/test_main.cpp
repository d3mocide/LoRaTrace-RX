#include <unity.h>

#include <string.h>

#include "../../src/meshtastic_identity.h"
#include "../../src/meshcore_identity.h"

void test_nodeinfo_payload_decodes_identity_fields() {
    uint8_t user[64] = {0x12, 5, 'A', 'l', 'p', 'h', 'a', 0x1A, 2, 'A', 'L', 0x42, 32};
    for (uint8_t i = 0; i < 32; i++) user[13 + i] = i;
    uint8_t data[80] = {0x08, 0x04, 0x12, 45};
    memcpy(data + 4, user, 45);

    NodeIdentity identity;
    TEST_ASSERT_TRUE(meshtasticParseNodeIdentityPayload(data, 49, 0x1234ABCDu, 9000, 49, identity));
    TEST_ASSERT_EQUAL_HEX32(0x1234ABCDu, identity.node_id);
    TEST_ASSERT_EQUAL_STRING("Alpha", identity.long_name);
    TEST_ASSERT_EQUAL_STRING("AL", identity.short_name);
    TEST_ASSERT_TRUE(identity.has_public_key);
    TEST_ASSERT_EQUAL_HEX8(0x00, identity.public_key[0]);
    TEST_ASSERT_EQUAL_HEX8(0x1F, identity.public_key[31]);
}

void test_non_nodeinfo_payload_is_not_an_identity() {
    const uint8_t data[] = {0x08, 0x01, 0x12, 0x01, 0x78};
    NodeIdentity identity;
    TEST_ASSERT_FALSE(meshtasticParseNodeIdentityPayload(data, sizeof(data), 1, 0, 5, identity));
}

void test_node_name_controls_are_safe_for_single_line_csv() {
    const uint8_t data[] = {0x08, 0x04, 0x12, 7,
                            0x12, 5, 'A', '\n', 'B', '\r', 'C'};
    NodeIdentity identity;
    TEST_ASSERT_TRUE(meshtasticParseNodeIdentityPayload(data, sizeof(data), 1, 0,
                                                         sizeof(data), identity));
    TEST_ASSERT_EQUAL_STRING("A B C", identity.long_name);
}

void test_nodes_csv_quotes_names_and_keeps_key() {
    NodeIdentity identity;
    identity.node_id = 0x1234ABCDu;
    identity.rx_millis = 700;
    identity.raw_len = 49;
    strcpy(identity.long_name, "A, \"Node\"");
    strcpy(identity.short_name, "AN");
    identity.has_public_key = true;
    memset(identity.public_key, 0xAB, sizeof(identity.public_key));
    char row[NODE_IDENTITY_CSV_MAX_ROW];
    TEST_ASSERT_TRUE(nodeIdentityFormatCsv(identity, row, sizeof(row), "t", false, 0, 0, 0, 7) > 0);
    TEST_ASSERT_NOT_NULL(strstr(row, "!1234abcd,\"\",\"A, \"\"Node\"\"\",\"AN\",abab"));
    TEST_ASSERT_NOT_NULL(strstr(row, ",49"));
}

void test_meshcore_advert_decodes_public_key_type_and_name() {
    uint8_t frame[118] = {};
    frame[0] = 0x11;  // v1, advert, flood route
    frame[1] = 0x41;  // one 2-byte path hash
    frame[2] = 0xAA;
    frame[3] = 0xBB;
    for (uint8_t i = 0; i < NODE_IDENTITY_PUBLIC_KEY_LEN; i++) frame[4 + i] = (uint8_t)(0xA0 + i);
    const size_t appData = 4 + MESHCORE_ADVERT_FIXED_BYTES;
    frame[appData] = 0x92;  // repeater + position + name
    frame[appData + 9] = 'N';
    frame[appData + 10] = 'o';
    frame[appData + 11] = 'd';
    frame[appData + 12] = 'e';
    frame[appData + 13] = '\n';

    Detection det = {};
    det.profile = (uint8_t)MissionProfile::MESHCORE;
    det.rx_millis = 1234;
    TEST_ASSERT_TRUE(detectionSetRawPacket(det, frame, sizeof(frame)));
    NodeIdentity identity;
    TEST_ASSERT_TRUE(meshcoreDecodeAdvertIdentity(det, identity));
    TEST_ASSERT_EQUAL_HEX8(0xA0, identity.node_id);
    TEST_ASSERT_EQUAL_HEX8(0xBF, identity.public_key[31]);
    TEST_ASSERT_TRUE(identity.has_public_key);
    TEST_ASSERT_EQUAL_UINT8(2, identity.node_type);
    TEST_ASSERT_EQUAL_STRING("Node ", identity.long_name);

    char row[NODE_IDENTITY_CSV_MAX_ROW];
    TEST_ASSERT_TRUE(nodeIdentityFormatCsv(identity, row, sizeof(row), "t", false, 0, 0, 0, 7) > 0);
    TEST_ASSERT_NOT_NULL(strstr(row, "meshcore,#a0,\"repeater\",\"Node \""));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_nodeinfo_payload_decodes_identity_fields);
    RUN_TEST(test_non_nodeinfo_payload_is_not_an_identity);
    RUN_TEST(test_node_name_controls_are_safe_for_single_line_csv);
    RUN_TEST(test_nodes_csv_quotes_names_and_keeps_key);
    RUN_TEST(test_meshcore_advert_decodes_public_key_type_and_name);
    return UNITY_END();
}
