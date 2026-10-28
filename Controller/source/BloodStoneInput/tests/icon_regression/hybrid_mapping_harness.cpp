#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <tuple>

enum class Icon : std::size_t {
    A, B, X, Y, LB, RB, LT, RT, DPU, DPD, DPL, DPR,
    LS_UP, LS_DOWN, LS_LEFT, LS_RIGHT, LSC,
    RS_UP, RS_DOWN, RS_ALL, RS_RIGHT, RSC, START, BACK, COUNT
};

enum class Table { Keyboard, Mouse };
struct Carrier { Table table; int index; int x; int y; int x2; int y2; };

constexpr std::array<Carrier, static_cast<std::size_t>(Icon::COUNT)> kCarriers{{
    {Table::Keyboard, 27, 4, 7, -1, -1},
    {Table::Keyboard, 57, 1, 2, -1, -1},
    {Table::Keyboard, 14, 3, 2, -1, -1},
    {Table::Keyboard, 56, 0, 7, -1, -1},
    {Table::Keyboard, 121, 0, 1, -1, -1},
    {Table::Keyboard, 126, 1, 1, -1, -1},
    {Table::Keyboard, 122, 2, 3, -1, -1},
    {Table::Keyboard, 123, 3, 3, -1, -1},
    {Table::Keyboard, 120, 0, 3, -1, -1},
    {Table::Keyboard, 125, 1, 3, -1, -1},
    {Table::Keyboard, 127, 2, 1, -1, -1},
    {Table::Keyboard, 128, 3, 1, -1, -1},
    {Table::Keyboard, 1, 0, 5, -1, -1},
    {Table::Keyboard, 4, 2, 6, -1, -1},
    {Table::Keyboard, 69, 0, 2, -1, -1},
    {Table::Keyboard, 68, 2, 2, -1, -1},
    {Table::Keyboard, 41, 6, 7, -1, -1},
    {Table::Keyboard, 28, 7, 7, -1, -1},
    {Table::Mouse, 0, 4, 6, -1, -1},
    {Table::Mouse, 1, 3, 6, -1, -1},
    {Table::Mouse, 2, 1, 5, -1, -1},
    {Table::Mouse, 5, 5, 6, -1, -1},
    {Table::Keyboard, 0, 5, 7, -1, -1},
    {Table::Mouse, 6, 6, 6, -1, -1},
}};


struct StyleMap {
    const char* name;
    const char* a;
    const char* b;
    const char* x;
    const char* y;
    const char* start;
    const char* back;
};

constexpr std::array<StyleMap, 3> kStyles{{
    {"Xbox", "A.png", "B.png", "X.png", "Y.png", "START.png", "BACK.png"},
    {"PS5", "Cross.png", "Circle.png", "Square.png", "Triangle.png", "Options.png", "Touch Pad Press.png"},
    {"Switch", "B.png", "A.png", "Y.png", "X.png", "Plus.png", "Minus.png"},
}};

enum class ActionSemantic { None, CoverDirection, HackingQte };

constexpr Icon resolveAction38_39(std::uintptr_t directCaller,
                                  int actionId,
                                  ActionSemantic& semantic) {
    constexpr std::uintptr_t buildA = 0x00C32B7E;
    constexpr std::uintptr_t buildD = 0x00C32B5C;
    constexpr std::uintptr_t runtime = 0x00C75873;
    semantic = ActionSemantic::None;

    if (actionId != 38 && actionId != 39)
        return Icon::COUNT;
    if (directCaller == runtime) {
        semantic = ActionSemantic::HackingQte;
        return actionId == 38 ? Icon::A : Icon::X;
    }
    if (directCaller == buildA && actionId == 38) {
        semantic = ActionSemantic::CoverDirection;
        return Icon::LS_LEFT;
    }
    if (directCaller == buildD && actionId == 39) {
        semantic = ActionSemantic::CoverDirection;
        return Icon::LS_RIGHT;
    }
    return Icon::COUNT;
}

constexpr const Carrier& carrier(Icon icon) {
    return kCarriers.at(static_cast<std::size_t>(icon));
}

int main() {
    static_assert(kCarriers.size() == 24);
    static_assert(kStyles.size() == 3);
    assert(std::string(kStyles[0].name) == "Xbox");
    assert(std::string(kStyles[1].back) == "Touch Pad Press.png");
    assert(std::string(kStyles[1].start) == "Options.png");
    assert(std::string(kStyles[2].a) == "B.png");
    assert(std::string(kStyles[2].b) == "A.png");
    assert(std::string(kStyles[2].x) == "Y.png");
    assert(std::string(kStyles[2].y) == "X.png");
    assert(std::string(kStyles[2].start) == "Plus.png");
    assert(std::string(kStyles[2].back) == "Minus.png");

    assert(carrier(Icon::A).index == 27);       // RETURN
    assert(carrier(Icon::B).index == 57);       // CAPSLOCK
    assert(carrier(Icon::Y).index == 56);       // SPACE
    assert(carrier(Icon::START).index == 0);    // ESCAPE
    assert(carrier(Icon::LT).index == 122);     // ARROWLEFT
    assert(carrier(Icon::RT).index == 123);     // ARROWRIGHT
    assert(carrier(Icon::DPL).index == 127);    // INSERT
    assert(carrier(Icon::DPR).index == 128);    // DELETE
    assert(carrier(Icon::RS_ALL).table == Table::Mouse);
    assert(carrier(Icon::RS_ALL).index == 1);   // MOUSERIGHT static slot

    // v1.4.11 全局交换四个左摇杆方向载体。
    assert(carrier(Icon::LS_LEFT).index == 69);   // SCROLLLOCK
    assert(carrier(Icon::LS_LEFT).x == 0 && carrier(Icon::LS_LEFT).y == 2);
    assert(carrier(Icon::LS_RIGHT).index == 68);  // NUMLOCK
    assert(carrier(Icon::LS_RIGHT).x == 2 && carrier(Icon::LS_RIGHT).y == 2);
    assert(carrier(Icon::LS_UP).index == 1);      // k1
    assert(carrier(Icon::LS_UP).x == 0 && carrier(Icon::LS_UP).y == 5);
    assert(carrier(Icon::LS_DOWN).index == 4);    // k4
    assert(carrier(Icon::LS_DOWN).x == 2 && carrier(Icon::LS_DOWN).y == 6);

    ActionSemantic semantic = ActionSemantic::None;
    assert(resolveAction38_39(0x00C32B7E, 38, semantic) == Icon::LS_LEFT);
    assert(semantic == ActionSemantic::CoverDirection);
    assert(resolveAction38_39(0x00C32B5C, 39, semantic) == Icon::LS_RIGHT);
    assert(semantic == ActionSemantic::CoverDirection);
    assert(resolveAction38_39(0x00C75873, 38, semantic) == Icon::A);
    assert(semantic == ActionSemantic::HackingQte);
    assert(resolveAction38_39(0x00C75873, 39, semantic) == Icon::X);
    assert(semantic == ActionSemantic::HackingQte);
    assert(resolveAction38_39(0x00C32B7E, 39, semantic) == Icon::COUNT);

    std::set<std::tuple<int, int>> identities;
    for (const Carrier& c : kCarriers) {
        identities.emplace(static_cast<int>(c.table), c.index);
        assert(c.x >= 0 && c.x < 8 && c.y >= 0 && c.y < 8);
        assert(!(c.x == 3 && c.y == 7));
    }
    assert(identities.size() == kCarriers.size());

    std::cout
        << "hybrid mapping harness passed: carriers=24 cycle=8 context38_39=4"
        << " ls_swap=LEFT:SCROLLLOCK,RIGHT:NUMLOCK,UP:k1,DOWN:k4"
        << " rs_aim=RS_ALL"
        << " styles=Xbox,PS5,Switch ps5_back=TouchPadPress switch_abxy=physical"
        << " static=SPACE:Y,ESCAPE:START,LEFT:LT,RIGHT:RT"
        << " dynamic=A:RETURN,B:CAPSLOCK,DPL:INSERT,DPR:DELETE\n";
}
