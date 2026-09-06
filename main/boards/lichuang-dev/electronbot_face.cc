#include "electronbot_face.h"

#include <cstring>

#include "display/lvgl_display/lvgl_image.h"
#include "electronbot_face_assets.h"

namespace {

// xiaozhi 21 个情绪名 → ElectronBot 6 组表情（名称表与 grobot_eyes.cc kNames 同源，
// 加 kkzz 表情全集常见名兜底）。静态组（neutral）配眨眼偶发，由 LcdDisplay 定时器驱动。
const char* const kHappyNames[] = {
    "happy", "laughing", "funny", "loving", "delicious", "silly", "confident", "cool",
};
const char* const kSadNames[] = {"sad", "crying"};
const char* const kAngryNames[] = {"angry"};
const char* const kShockedNames[] = {"shocked", "surprised", "fearful"};
const char* const kDisdainNames[] = {"thinking", "confused", "kissy", "embarrassed", "winking"};
const char* const kNeutralNames[] = {"neutral", "relaxed", "sleepy", "idle", "staticstate"};

bool NameIn(const char* name, const char* const* list, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (std::strcmp(name, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

const char* ElectronBotEmojiCollection::MapEmotion(const char* name) {
    if (name == nullptr) {
        return "neutral";
    }
    if (NameIn(name, kHappyNames, sizeof(kHappyNames) / sizeof(kHappyNames[0]))) return "happy";
    if (NameIn(name, kSadNames, sizeof(kSadNames) / sizeof(kSadNames[0]))) return "sad";
    if (NameIn(name, kAngryNames, sizeof(kAngryNames) / sizeof(kAngryNames[0]))) return "angry";
    if (NameIn(name, kShockedNames, sizeof(kShockedNames) / sizeof(kShockedNames[0]))) return "shocked";
    if (NameIn(name, kDisdainNames, sizeof(kDisdainNames) / sizeof(kDisdainNames[0]))) return "disdain";
    return "neutral";
}

void ElectronBotEmojiCollection::AddRaw(const std::string& key, const uint8_t* data, size_t size) {
    // LvglRawImage 不拥有数据（包装只读字节）；collection 析构只 delete wrapper
    AddEmoji(key, new LvglRawImage(const_cast<uint8_t*>(data), size));
}

ElectronBotEmojiCollection::ElectronBotEmojiCollection() {
    AddRaw("happy", kFaceHappyLoop, kFaceHappyLoopSize);
    AddRaw("sad", kFaceSadLoop, kFaceSadLoopSize);
    AddRaw("angry", kFaceAngryLoop, kFaceAngryLoopSize);
    AddRaw("shocked", kFaceShockedLoop, kFaceShockedLoopSize);
    AddRaw("disdain", kFaceDisdainLoop, kFaceDisdainLoopSize);
    AddRaw("neutral", kFaceNeutralStatic, kFaceNeutralStaticSize);
    AddRaw("blink_once", kFaceBlinkOnce, kFaceBlinkOnceSize);
    AddRaw("blink_twice", kFaceBlinkTwice, kFaceBlinkTwiceSize);
}

const LvglImage* ElectronBotEmojiCollection::GetEmojiImage(const char* name) {
    // 眨眼键是内部资产，允许直取；情绪名一律先归一化再取，未知名兜底 neutral
    if (name != nullptr &&
        (std::strcmp(name, "blink_once") == 0 || std::strcmp(name, "blink_twice") == 0)) {
        return EmojiCollection::GetEmojiImage(name);
    }
    return EmojiCollection::GetEmojiImage(MapEmotion(name));
}
