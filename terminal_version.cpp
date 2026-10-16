/*
 * 深夜便利店 —— 一个跑在终端里的文字小游戏
 *
 * 玩法：全程只需要输入数字选项。
 * 三个主要分支，通向六个不同结局。
 *
 * 编译：g++ -std=c++17 -o game.exe main.cpp
 * 运行：game.exe
 */

#define NOMINMAX
#ifdef _WIN32
#include <windows.h>   // 只为了把终端切到 UTF-8，避免中文乱码
#endif

#include <iostream>
#include <string>
#include <limits>

using namespace std;

/* ==================== 通用小工具 ==================== */

// 让 Windows 终端按 UTF-8 显示中文
static void setupConsole() {
#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif
}

static void line(const string& text = "") {
    cout << text << '\n';
}

static void divider() {
    line("──────────────────────────────────────────────");
}

// 读一个 1~count 的数字，非法输入会重新问
static int askChoice(int count) {
    while (true) {
        cout << "\n你的选择 (1-" << count << ") > ";
        int choice = 0;
        if (cin >> choice && choice >= 1 && choice <= count) {
            return choice;
        }
        cin.clear();
        cin.ignore(numeric_limits<streamsize>::max(), '\n');
        line(">> 没有这个选项，重新选一次吧。");
    }
}

// 停顿一下，让玩家看完剧情（用管道喂输入时会自动跳过）
static void pauseGame() {
    line("\n(按回车继续…)");
    cin.ignore(numeric_limits<streamsize>::max(), '\n');
    cin.get();
}

// 统一格式的结局输出
static void ending(int index, const string& title, const string& text) {
    divider();
    line("【  结  局  " + to_string(index) + " / 6  】  " + title);
    divider();
    line(text);
    divider();
}

/* ==================== 分支 1：主动搭话 ==================== */

static void branchTalk() {
    divider();
    line("你走到冰柜前。他没有回头，却先开了口：");
    line("「你听得见我的话，对吧？二十年了，你是第一个。」");
    line("他说他是这家店从前的店长，1999 年的冬夜被困在了这里，");
    line("只要有人肯听他讲完那一晚的事，他就能离开。");
    line("");
    line("  1. 拉把椅子坐下，认真听完");
    line("  2. 打断他：「先生，一共十二块五。」");

    if (askChoice(2) == 1) {
        pauseGame();
        divider();
        line("他讲了很久。四点零七分，最后一个字落下，冰柜的灯闪了一下。");
        line("他站起身，认真地看了你一眼：「现在，轮到你了。」");
        line("第二天招牌换了老板，工资翻倍、福利极好——");
        line("只有一条规定：凌晨两点四十七分，绝对不能离开收银台。");
        ending(1, "接 棒 人", "你成了新的店长，也成了新的故事。");
    } else {
        pauseGame();
        divider();
        line("他愣住了，像是没听懂。然后慢慢转身，往门外走。");
        line("自动门关上的一瞬间，你听见街上有人喊了一声你的名字。");
        line("第二天，门口挂上一张寻人启事：1999 年冬夜失踪的店长。");
        ending(2, "铁 门 之 后", "有些门一旦关上，就不再打开。");
    }
}

/* ==================== 分支 2：装作没看见 ==================== */

static void branchIgnore() {
    divider();
    line("你低下头，把手里的泡面一排排码整齐。");
    line("脚步声在你身后停住了，空气里有很淡的、雪的味道。");
    line("");
    line("  1. 蹲下来，继续理最底层的货架");
    line("  2. 抬手点开收银台的监控画面");

    if (askChoice(2) == 1) {
        pauseGame();
        divider();
        line("你一直蹲到腿发麻。再抬头时，冰柜前已经没人了。");
        line("天亮交接班，收银机里多出一张 1999 年的一元纸币，边角还是潮的。");
        line("店长说：留着吧，那是收银机的找零。");
        ending(3, "天 亮 就 好", "有些顾客不需要被服务，只需要被无视。");
    } else {
        pauseGame();
        divider();
        line("屏幕上，你的身后站着一个人。");
        line("他离你很近，近到你能看清他没有呼吸——");
        line("你猛地回头。身后只有第三个货架，和整整齐齐的泡面。");
        line("再看屏幕，那个人还在，而且正对着镜头笑。");
        ending(4, "第 三 个 货 架", "摄像头不会说谎，说谎的是你身后的空气。");
    }
}

/* ==================== 分支 3：关灯跑路 ==================== */

static void branchRun() {
    divider();
    line("你伸手按向墙上的总电闸，指尖有点凉。");
    line("背后传来很轻的一句：「你确定要关灯吗？」");
    line("");
    line("  1. 拉下电闸，趁黑从后门跑");
    line("  2. 别管灯了，直接冲出正门");

    if (askChoice(2) == 1) {
        pauseGame();
        divider();
        line("黑暗里只剩冰柜的嗡鸣，和一声很长的叹息。");
        line("灯重新亮起时，店里空无一人，仿佛什么都没发生过。");
        line("你松了口气，低头看见自己的工牌——");
        line("照片是你，名字不是。");
        ending(5, "黑 灯", "你总算下班了。只是走出店门的那个人，不一定还是你。");
    } else {
        pauseGame();
        divider();
        line("你撞开玻璃门冲到大街上。冷风、路灯、空无一人的十字路口。");
        line("一只手拍上你的肩膀，是同事：「你站门口干嘛？快上课了。」");
        line("你回头看，便利店还在，钟表停在 2:47。");
        ending(6, "回 到 原 点", "你以为自己逃出去了，其实只是又站回了起点。");
    }
}

/* ==================== 主流程 ==================== */

int main() {
    setupConsole();

    divider();
    line("              深  夜  便  利  店");
    line("        —— 一个凌晨 2:47 的终端文字游戏 ——");
    divider();
    line("你在这家 24 小时便利店值夜班，现在是凌晨 2:47。");
    line("门口的自动门「叮咚」响了，进来一位顾客。");
    line("他在冰柜前站了十分钟，一动不动——");
    line("而且，他脚下的地砖上，没有影子。");
    line("");
    line("你打算怎么做？");
    line("  1. 走过去，主动搭话");
    line("  2. 装作没看见，继续理货架");
    line("  3. 关掉灯，从后门跑掉");

    switch (askChoice(3)) {
    case 1:
        branchTalk();
        break;
    case 2:
        branchIgnore();
        break;
    default:
        branchRun();
        break;
    }

    line("感谢游玩，下次夜班再见。");
    return 0;
}
