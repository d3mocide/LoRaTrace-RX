// MenuState (ui_menu.h) is the Phase 6 menu state machine, generalized to
// real nesting 2026-08-25 (see ui_menu.h's own header comment for the full
// history — it started as a deliberate two-level design, then grew a third
// when the operator asked to move Brightness/idle-dim under a nested
// "System > Display > ..." grouping). Runs on the host (`pio test -e
// native`), no hardware/display needed — see platformio.ini [env:native].

#include <unity.h>

#include "../../src/ui_menu.h"

namespace {

// Mirrors the real ui_task.cpp table shape as of the Phase 8 Probe revision:
// Trace(ACTION) / Profile(GROUP) / System(GROUP: WiFi,
// Debug, Display(GROUP: Brightness(SLIDER), Idle dim(ACTION))). Without
// depending on ui_task.cpp itself, so this test exercises the same
// structure production code uses — including the one place nesting two
// GROUPs deep actually happens today.
constexpr MenuItem kProfileGroup[] = {
    {"Meshtastic", ItemKind::ACTION, MenuAction::SELECT_MESHTASTIC, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"MeshCore", ItemKind::ACTION, MenuAction::SELECT_MESHCORE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
};

constexpr MenuItem kDisplayGroup[] = {
    {"Brightness", ItemKind::SLIDER, MenuAction::NONE, MenuAction::BRIGHTNESS_UP, MenuAction::BRIGHTNESS_DOWN, nullptr, 0},
    {"Idle dim", ItemKind::ACTION, MenuAction::IDLE_TIMEOUT_CYCLE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
};

constexpr MenuItem kSystemGroup[] = {
    {"WiFi", ItemKind::ACTION, MenuAction::WIFI_TOGGLE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"Debug", ItemKind::ACTION, MenuAction::DEBUG_TOGGLE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"Display", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, kDisplayGroup, 2},
};

constexpr MenuItem kRoots[] = {
    {"Trace:", ItemKind::ACTION, MenuAction::TRACE_TOGGLE, MenuAction::NONE, MenuAction::NONE, nullptr, 0},
    {"Profile", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, kProfileGroup, 2},
    {"System", ItemKind::GROUP, MenuAction::NONE, MenuAction::NONE, MenuAction::NONE, kSystemGroup, 3},
};
constexpr uint8_t kRootCount = 3;

} // namespace

void test_starts_closed() {
    MenuState menu(kRoots, kRootCount);
    TEST_ASSERT_FALSE(menu.isOpen());
}

void test_open_lands_on_first_root_item() {
    MenuState menu(kRoots, kRootCount);
    menu.open();
    TEST_ASSERT_TRUE(menu.isOpen());
    TEST_ASSERT_EQUAL_UINT8(1, menu.depth());
    TEST_ASSERT_EQUAL_UINT8(0, menu.currentIndex());
    TEST_ASSERT_FALSE(menu.inSlider());
}

void test_root_next_prev_wrap() {
    MenuState menu(kRoots, kRootCount);
    menu.open();
    TEST_ASSERT_TRUE(MenuAction::NONE == menu.handle(KeyAction::NEXT));
    TEST_ASSERT_EQUAL_UINT8(1, menu.currentIndex()); // "Profile"
    TEST_ASSERT_TRUE(MenuAction::NONE == menu.handle(KeyAction::NEXT));
    TEST_ASSERT_EQUAL_UINT8(2, menu.currentIndex()); // "System"
    // Wraps past the last root item back to the first.
    TEST_ASSERT_TRUE(MenuAction::NONE == menu.handle(KeyAction::NEXT));
    TEST_ASSERT_EQUAL_UINT8(0, menu.currentIndex());
    // Wraps the other direction too.
    TEST_ASSERT_TRUE(MenuAction::NONE == menu.handle(KeyAction::PREV));
    TEST_ASSERT_EQUAL_UINT8(2, menu.currentIndex());
}

void test_select_trace_root_fires_directly() {
    MenuState menu(kRoots, kRootCount);
    menu.open();
    const MenuAction fired = menu.handle(KeyAction::SELECT);
    TEST_ASSERT_TRUE(MenuAction::TRACE_TOGGLE == fired);
    TEST_ASSERT_EQUAL_UINT8(1, menu.depth());
}

void test_select_profile_opens_group_at_first_item() {
    MenuState menu(kRoots, kRootCount);
    menu.open();
    menu.handle(KeyAction::NEXT); // "Profile"
    const MenuAction fired = menu.handle(KeyAction::SELECT);
    TEST_ASSERT_TRUE(MenuAction::NONE == fired); // opening a group fires nothing
    TEST_ASSERT_EQUAL_UINT8(2, menu.depth());
    TEST_ASSERT_EQUAL_UINT8(0, menu.currentIndex());
    TEST_ASSERT_EQUAL_UINT8(1, menu.breadcrumbCount());
    TEST_ASSERT_EQUAL_STRING("Profile", menu.breadcrumbLabel(0));
}

void test_select_profile_group_items_fire_profile_actions() {
    MenuState menu(kRoots, kRootCount);
    menu.open();
    menu.handle(KeyAction::NEXT);   // "Profile"
    menu.handle(KeyAction::SELECT); // enter Profile's list, at "Meshtastic"
    TEST_ASSERT_TRUE(MenuAction::SELECT_MESHTASTIC == menu.handle(KeyAction::SELECT));
    TEST_ASSERT_EQUAL_UINT8(2, menu.depth()); // firing an ACTION doesn't change depth
    menu.handle(KeyAction::NEXT); // "MeshCore"
    TEST_ASSERT_TRUE(MenuAction::SELECT_MESHCORE == menu.handle(KeyAction::SELECT));
}

void test_nested_group_opens_one_level_deeper() {
    // System -> Display is a GROUP inside a GROUP — the actual case this
    // generalization exists for.
    MenuState menu(kRoots, kRootCount);
    menu.open();
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::NEXT);
    // "System"
    menu.handle(KeyAction::SELECT); // enter System's list
    TEST_ASSERT_EQUAL_UINT8(2, menu.depth());
    menu.handle(KeyAction::NEXT); // "Debug"
    menu.handle(KeyAction::NEXT); // "Display"
    const MenuAction fired = menu.handle(KeyAction::SELECT);
    TEST_ASSERT_TRUE(MenuAction::NONE == fired);
    TEST_ASSERT_EQUAL_UINT8(3, menu.depth());
    TEST_ASSERT_EQUAL_UINT8(0, menu.currentIndex()); // Display's list starts at "Brightness"
    TEST_ASSERT_EQUAL_UINT8(2, menu.breadcrumbCount());
    TEST_ASSERT_EQUAL_STRING("System", menu.breadcrumbLabel(0));
    TEST_ASSERT_EQUAL_STRING("Display", menu.breadcrumbLabel(1));
}

void test_slider_reachable_from_nested_group() {
    MenuState menu(kRoots, kRootCount);
    menu.open();
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::SELECT); // System's list
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::SELECT); // Display's list, at "Brightness"
    const MenuAction fired = menu.handle(KeyAction::SELECT); // enter the slider
    TEST_ASSERT_TRUE(MenuAction::NONE == fired);
    TEST_ASSERT_TRUE(menu.inSlider());
    TEST_ASSERT_EQUAL_UINT8(3, menu.depth()); // unchanged — slider doesn't add depth
    TEST_ASSERT_TRUE(MenuAction::BRIGHTNESS_UP == menu.handle(KeyAction::NEXT));
    TEST_ASSERT_TRUE(menu.inSlider()); // stays inside, unlike a GROUP item firing
    TEST_ASSERT_TRUE(MenuAction::BRIGHTNESS_DOWN == menu.handle(KeyAction::PREV));
}

void test_slider_back_returns_to_its_list_not_up_a_level() {
    MenuState menu(kRoots, kRootCount);
    menu.open();
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::SELECT);
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::SELECT); // Display's list
    menu.handle(KeyAction::SELECT); // enter Brightness's slider
    menu.handle(KeyAction::BACK);
    TEST_ASSERT_FALSE(menu.inSlider());
    TEST_ASSERT_EQUAL_UINT8(3, menu.depth()); // still Display's list, not System's
    TEST_ASSERT_EQUAL_UINT8(0, menu.currentIndex()); // still on "Brightness"
}

void test_slider_select_also_returns_to_its_list() {
    // Enter should confirm a value, not just ESC out of it (operator
    // request, 2026-08-29) -- same "one step back" shape as BACK, not a
    // second, different behavior.
    MenuState menu(kRoots, kRootCount);
    menu.open();
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::SELECT);
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::SELECT); // Display's list
    menu.handle(KeyAction::SELECT); // enter Brightness's slider
    const MenuAction fired = menu.handle(KeyAction::SELECT); // confirm/exit
    TEST_ASSERT_TRUE(MenuAction::NONE == fired);
    TEST_ASSERT_FALSE(menu.inSlider());
    TEST_ASSERT_EQUAL_UINT8(3, menu.depth()); // still Display's list, not System's
    TEST_ASSERT_EQUAL_UINT8(0, menu.currentIndex()); // still on "Brightness"
}

void test_idle_timeout_cycle_fires_and_stays_in_list() {
    MenuState menu(kRoots, kRootCount);
    menu.open();
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::SELECT);
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::SELECT); // Display's list
    menu.handle(KeyAction::NEXT);   // "Idle dim"
    const MenuAction fired = menu.handle(KeyAction::SELECT);
    TEST_ASSERT_TRUE(MenuAction::IDLE_TIMEOUT_CYCLE == fired);
    TEST_ASSERT_EQUAL_UINT8(3, menu.depth());
}

void test_group_next_prev_wrap() {
    MenuState menu(kRoots, kRootCount);
    menu.open();
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::SELECT); // now inside System's 3-item list
    TEST_ASSERT_EQUAL_UINT8(0, menu.currentIndex());
    menu.handle(KeyAction::NEXT);
    TEST_ASSERT_EQUAL_UINT8(1, menu.currentIndex());
    menu.handle(KeyAction::NEXT);
    TEST_ASSERT_EQUAL_UINT8(2, menu.currentIndex());
    // Wraps past the last item back to the first.
    menu.handle(KeyAction::NEXT);
    TEST_ASSERT_EQUAL_UINT8(0, menu.currentIndex());
    // Wraps the other direction too.
    menu.handle(KeyAction::PREV);
    TEST_ASSERT_EQUAL_UINT8(2, menu.currentIndex());
}

void test_select_group_item_fires_its_action_and_stays_in_list() {
    MenuState menu(kRoots, kRootCount);
    menu.open();
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::SELECT); // enter System
    menu.handle(KeyAction::NEXT);   // move to "Debug"
    const MenuAction fired = menu.handle(KeyAction::SELECT);
    TEST_ASSERT_TRUE(MenuAction::DEBUG_TOGGLE == fired);
    TEST_ASSERT_EQUAL_UINT8(2, menu.depth());
}

void test_back_walks_up_one_level_at_a_time() {
    MenuState menu(kRoots, kRootCount);
    menu.open();
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::SELECT); // depth 2 (System)
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::SELECT); // depth 3 (Display)
    menu.handle(KeyAction::BACK);
    TEST_ASSERT_EQUAL_UINT8(2, menu.depth());
    TEST_ASSERT_TRUE(menu.isOpen());
    TEST_ASSERT_EQUAL_UINT8(2, menu.currentIndex()); // still remembers "Display" in System's list
    menu.handle(KeyAction::BACK);
    TEST_ASSERT_EQUAL_UINT8(1, menu.depth());
    TEST_ASSERT_EQUAL_UINT8(2, menu.currentIndex()); // still remembers "System" at root
    menu.handle(KeyAction::BACK);
    TEST_ASSERT_FALSE(menu.isOpen());
}

void test_jump_and_none_keys_are_no_ops_at_every_level() {
    MenuState menu(kRoots, kRootCount);
    menu.open();
    TEST_ASSERT_TRUE(MenuAction::NONE == menu.handle(KeyAction::JUMP_3));
    TEST_ASSERT_TRUE(MenuAction::NONE == menu.handle(KeyAction::NONE));
    TEST_ASSERT_EQUAL_UINT8(0, menu.currentIndex()); // untouched
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::NEXT);
    menu.handle(KeyAction::SELECT); // enter System
    TEST_ASSERT_TRUE(MenuAction::NONE == menu.handle(KeyAction::JUMP_1));
    TEST_ASSERT_EQUAL_UINT8(2, menu.depth());
    TEST_ASSERT_EQUAL_UINT8(0, menu.currentIndex()); // untouched
}

void test_reopen_after_close_resets_to_first_root_item() {
    MenuState menu(kRoots, kRootCount);
    menu.open();
    menu.handle(KeyAction::NEXT); // index now 1
    menu.handle(KeyAction::BACK); // closes
    menu.open();
    TEST_ASSERT_EQUAL_UINT8(1, menu.depth());
    TEST_ASSERT_EQUAL_UINT8(0, menu.currentIndex());
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_starts_closed);
    RUN_TEST(test_open_lands_on_first_root_item);
    RUN_TEST(test_root_next_prev_wrap);
    RUN_TEST(test_select_trace_root_fires_directly);
    RUN_TEST(test_select_profile_opens_group_at_first_item);
    RUN_TEST(test_select_profile_group_items_fire_profile_actions);
    RUN_TEST(test_nested_group_opens_one_level_deeper);
    RUN_TEST(test_slider_reachable_from_nested_group);
    RUN_TEST(test_slider_back_returns_to_its_list_not_up_a_level);
    RUN_TEST(test_slider_select_also_returns_to_its_list);
    RUN_TEST(test_idle_timeout_cycle_fires_and_stays_in_list);
    RUN_TEST(test_group_next_prev_wrap);
    RUN_TEST(test_select_group_item_fires_its_action_and_stays_in_list);
    RUN_TEST(test_back_walks_up_one_level_at_a_time);
    RUN_TEST(test_jump_and_none_keys_are_no_ops_at_every_level);
    RUN_TEST(test_reopen_after_close_resets_to_first_root_item);
    return UNITY_END();
}
