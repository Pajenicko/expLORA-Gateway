/**
 * Native unit tests for LazySlots (the sensor tables, Data/LazySlots.h).
 *
 * Run:   pio test -e native
 */

#include <unity.h>
#include "../../src/Data/LazySlots.h"

void setUp(void) {}
void tearDown(void) {}

struct Item
{
    static int alive;
    int value = 7;
    Item() { alive++; }
    Item(const Item &o) : value(o.value) { alive++; }
    Item &operator=(const Item &) = default;
    ~Item() { alive--; }
};
int Item::alive = 0;

static void test_starts_empty(void)
{
    LazySlots<Item, 4> s;
    TEST_ASSERT_EQUAL_UINT32(4, s.capacity());
    TEST_ASSERT_EQUAL_UINT32(0, s.allocatedCount());
    TEST_ASSERT_EQUAL_INT(0, Item::alive);
    for (size_t i = 0; i < 5; i++)
    {
        TEST_ASSERT_FALSE(s.allocated(i));
    }
}

static void test_ensure_allocates_once(void)
{
    {
        LazySlots<Item, 4> s;
        TEST_ASSERT_TRUE(s.ensure(2));
        TEST_ASSERT_TRUE(s.allocated(2));
        TEST_ASSERT_FALSE(s.allocated(1));
        TEST_ASSERT_EQUAL_INT(7, s[2].value);
        s[2].value = 42;
        Item *p = &s[2];
        TEST_ASSERT_TRUE(s.ensure(2)); // kept, not re-created
        TEST_ASSERT_EQUAL_PTR(p, &s[2]);
        TEST_ASSERT_EQUAL_INT(42, s[2].value);
        TEST_ASSERT_EQUAL_UINT32(1, s.allocatedCount());
        TEST_ASSERT_EQUAL_INT(1, Item::alive);
        TEST_ASSERT_FALSE(s.ensure(4)); // out of range
        TEST_ASSERT_EQUAL_UINT32(1, s.allocatedCount());
        const LazySlots<Item, 4> &c = s;
        TEST_ASSERT_EQUAL_INT(42, c[2].value);
    }
    TEST_ASSERT_EQUAL_INT(0, Item::alive); // freed with the table
}

static void test_reuse_keeps_slot(void)
{
    LazySlots<Item, 3> s;
    for (size_t i = 0; i < 3; i++)
    {
        TEST_ASSERT_TRUE(s.ensure(i));
        s[i].value = (int)i;
    }
    Item *p = &s[1];
    s[1] = Item(); // a deleted entry reset in place
    TEST_ASSERT_EQUAL_PTR(p, &s[1]);
    TEST_ASSERT_EQUAL_INT(7, s[1].value);
    s[0] = s[2]; // a swap-remove copies the last entry down
    TEST_ASSERT_EQUAL_INT(2, s[0].value);
    TEST_ASSERT_EQUAL_UINT32(3, s.allocatedCount());
    TEST_ASSERT_EQUAL_INT(3, Item::alive);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_starts_empty);
    RUN_TEST(test_ensure_allocates_once);
    RUN_TEST(test_reuse_keeps_slot);
    return UNITY_END();
}
