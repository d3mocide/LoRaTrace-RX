// MenuState (ui_menu.h) is the Phase 6 replacement for Phase 5's flat,
// fixed-size menu — see ui_menu.h's own header comment for why. Runs on the
// host (`pio test -e native`), no hardware/display needed — see
// platformio.ini [env:native].

#include <unity.h>

#include "../../src/ui_menu.h"

namespace {

// Mirrors the real ui_task.cpp table shape as of the 2026-08-25 "Mesh Trace
// is a group" revision (BRAND.md, ui_menu.h's MenuAction comment): both
// root rows are GROUP now — "Mesh Trace" opens onto its two sub-profiles
// instead of firing a cycle-on-select action, same as "System" already did.
// Without depending on ui_task.cpp itself, so this test exercises the same
// structure production code uses.
constexpr MenuEntry kMeshTraceGroup[] = {
    {"Meshtastic", MenuAction::SELECT_MESHTASTIC},
    {"MeshCore", MenuAction::SELECT_MESHCORE},
};

constexpr MenuEntry kSystemGroup[] = {
    {"WiFi", MenuAction::WIFI_TOGGLE},
    {"Debug", MenuAction::DEBUG_TOGGLE},
};

constexpr RootEntry kRoots[] = {
    {"Mesh Trace", RootKind::GROUP, MenuAction::NONE, kMeshTraceGroup, 2},
    {"System", RootKind::GROUP, MenuAction::NONE, kSystemGroup, 2},
};
constexpr uint8_t kRootCount = 2;

// No root row in production is DIRECT any more (both are GROUP, above) —
// but MenuState still documents and supports a DIRECT root kind (see
// ui_menu.h's RootEntry/RootKind comments), so this synthetic single-row
// fixture exists purely to keep that branch covered. The action value
// carried doesn't matter for what this fixture tests (that a DIRECT row
// fires immediately and stays at ROOT level); WIFI_TOGGLE is reused rather
// than inventing a test-only MenuAction.
constexpr RootEntry kDirectRoots[] = {
    {"Direct", RootKind::DIRECT, MenuAction::WIFI_TOGGLE, nullptr, 0},
};
constexpr uint8_t kDirectRootCount = 1;

} // namespace

void test_starts_closed() {
    MenuState menu(kRoots, kRootCount);
    TEST_ASSERT_FALSE(menu.isOpen());
    TEST_ASSERT_TRUE(MenuLevel::CLOSED == menu.level());
}

void test_open_lands_on_first_root_item() {
    MenuState menu(kRoots, kRootCount);
    menu.open();
    TEST_ASSERT_TRUE(menu.isOpen());
    TEST_ASSERT_TRUE(MenuLevel::ROOT == menu.level());
    TEST_ASSERT_EQUAL_UINT8(0, menu.rootIndex());
}

void test_root_next_prev_wrap() {
    MenuState menu(kRoots, kRootCount);
    menu.open();
    TEST_ASSERT_TRUE(MenuAction::NONE == menu.handle(KeyAction::NEXT));
    TEST_ASSERT_EQUAL_UINT8(1, menu.rootIndex());
    // Wraps past the last root item back to the first.
    TEST_ASSERT_TRUE(MenuAction::NONE == menu.handle(KeyAction::NEXT));
    TEST_ASSERT_EQUAL_UINT8(0, menu.rootIndex());
    // Wraps the other direction too.
    TEST_ASSERT_TRUE(MenuAction::NONE == menu.handle(KeyAction::PREV));
    TEST_ASSERT_EQUAL_UINT8(1, menu.rootIndex());
}

void test_select_direct_root_fires_action_and_stays_at_root() {
    MenuState menu(kDirectRoots, kDirectRootCount);
    menu.open(); // rootIndex 0 == "Direct", a DIRECT row
    const MenuAction fired = menu.handle(KeyAction::SELECT);
    TEST_ASSERT_TRUE(MenuAction::WIFI_TOGGLE == fired);
    TEST_ASSERT_TRUE(MenuLevel::ROOT == menu.level());
}

void test_select_group_root_opens_group_at_first_item() {
    MenuState menu(kRoots, kRootCount);
    menu.open();
    menu.handle(KeyAction::NEXT); // move to "System", a GROUP row
    const MenuAction fired = menu.handle(KeyAction::SELECT);
    TEST_ASSERT_TRUE(MenuAction::NONE == fired); // opening a group fires nothing
    TEST_ASSERT_TRUE(MenuLevel::GROUP == menu.level());
    TEST_ASSERT_EQUAL_UINT8(0, menu.groupIndex());
}

void test_select_mesh_trace_root_opens_group_at_first_item() {
    // rootIndex 0 == "Mesh Trace" itself is a GROUP row now (2026-08-25) —
    // this is the case test_select_direct_root_... used to cover before
    // that revision, so it's worth its own check on the real kRoots shape.
    MenuState menu(kRoots, kRootCount);
    menu.open();
    const MenuAction fired = menu.handle(KeyAction::SELECT);
    TEST_ASSERT_TRUE(MenuAction::NONE == fired);
    TEST_ASSERT_TRUE(MenuLevel::GROUP == menu.level());
    TEST_ASSERT_EQUAL_UINT8(0, menu.groupIndex());
}

void test_select_mesh_trace_group_items_fire_sub_profile_actions() {
    MenuState menu(kRoots, kRootCount);
    menu.open();
    menu.handle(KeyAction::SELECT); // enter Mesh Trace's group, at "Meshtastic"
    TEST_ASSERT_TRUE(MenuAction::SELECT_MESHTASTIC == menu.handle(KeyAction::SELECT));
    TEST_ASSERT_TRUE(MenuLevel::GROUP == menu.level());
    menu.handle(KeyAction::NEXT); // "MeshCore"
    TEST_ASSERT_TRUE(MenuAction::SELECT_MESHCORE == menu.handle(KeyAction::SELECT));
}

void test_group_next_prev_wrap() {
    MenuState menu(kRoots, kRootCount);
    menu.open();
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::SELECT); // now inside System's 2-item group
    TEST_ASSERT_EQUAL_UINT8(0, menu.groupIndex());
    menu.handle(KeyAction::NEXT);
    TEST_ASSERT_EQUAL_UINT8(1, menu.groupIndex());
    // Wraps past the last group item back to the first.
    menu.handle(KeyAction::NEXT);
    TEST_ASSERT_EQUAL_UINT8(0, menu.groupIndex());
}

void test_select_group_item_fires_its_action_and_stays_in_group() {
    MenuState menu(kRoots, kRootCount);
    menu.open();
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::SELECT); // enter System
    menu.handle(KeyAction::NEXT);   // move to "Debug"
    const MenuAction fired = menu.handle(KeyAction::SELECT);
    TEST_ASSERT_TRUE(MenuAction::DEBUG_TOGGLE == fired);
    TEST_ASSERT_TRUE(MenuLevel::GROUP == menu.level());
}

void test_back_from_group_returns_to_root_not_closed() {
    MenuState menu(kRoots, kRootCount);
    menu.open();
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::SELECT); // enter System
    menu.handle(KeyAction::BACK);
    TEST_ASSERT_TRUE(MenuLevel::ROOT == menu.level());
    TEST_ASSERT_TRUE(menu.isOpen());
    // Still remembers which root row it came back to.
    TEST_ASSERT_EQUAL_UINT8(1, menu.rootIndex());
}

void test_back_from_root_closes_menu() {
    MenuState menu(kRoots, kRootCount);
    menu.open();
    menu.handle(KeyAction::BACK);
    TEST_ASSERT_FALSE(menu.isOpen());
    TEST_ASSERT_TRUE(MenuLevel::CLOSED == menu.level());
}

void test_jump_and_none_keys_are_no_ops_at_every_level() {
    MenuState menu(kRoots, kRootCount);
    menu.open();
    TEST_ASSERT_TRUE(MenuAction::NONE == menu.handle(KeyAction::JUMP_3));
    TEST_ASSERT_TRUE(MenuAction::NONE == menu.handle(KeyAction::NONE));
    TEST_ASSERT_EQUAL_UINT8(0, menu.rootIndex()); // untouched
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::SELECT); // enter System
    TEST_ASSERT_TRUE(MenuAction::NONE == menu.handle(KeyAction::JUMP_1));
    TEST_ASSERT_TRUE(MenuLevel::GROUP == menu.level());
    TEST_ASSERT_EQUAL_UINT8(0, menu.groupIndex()); // untouched
}

void test_reopen_after_close_resets_to_first_root_item() {
    MenuState menu(kRoots, kRootCount);
    menu.open();
    menu.handle(KeyAction::NEXT); // rootIndex now 1
    menu.handle(KeyAction::BACK); // closes
    menu.open();
    TEST_ASSERT_EQUAL_UINT8(0, menu.rootIndex());
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_starts_closed);
    RUN_TEST(test_open_lands_on_first_root_item);
    RUN_TEST(test_root_next_prev_wrap);
    RUN_TEST(test_select_direct_root_fires_action_and_stays_at_root);
    RUN_TEST(test_select_group_root_opens_group_at_first_item);
    RUN_TEST(test_select_mesh_trace_root_opens_group_at_first_item);
    RUN_TEST(test_select_mesh_trace_group_items_fire_sub_profile_actions);
    RUN_TEST(test_group_next_prev_wrap);
    RUN_TEST(test_select_group_item_fires_its_action_and_stays_in_group);
    RUN_TEST(test_back_from_group_returns_to_root_not_closed);
    RUN_TEST(test_back_from_root_closes_menu);
    RUN_TEST(test_jump_and_none_keys_are_no_ops_at_every_level);
    RUN_TEST(test_reopen_after_close_resets_to_first_root_item);
    return UNITY_END();
}
