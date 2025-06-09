#include "gui_main.hpp"

#include "dir_iterator.hpp"

#include <json.hpp>
using json = nlohmann::json;

constexpr const char *const amsContentsPath = "/atmosphere/contents";
constexpr const char *const boot2FlagFormat = "/atmosphere/contents/%016lX/flags/boot2.flag";
constexpr const char *const boot2FlagFolder = "/atmosphere/contents/%016lX/flags";

static char pathBuffer[FS_MAX_PATH];

constexpr const char *const descriptions[2][2] = {
    [0] = {
        [0] = "Off | \uE098",
        [1] = "Off | \uE0F4",
    },
    [1] = {
        [0] = "On | \uE098",
        [1] = "On | \uE0F4",
    },
};

GuiMain::GuiMain() {
    Result rc = fsOpenSdCardFileSystem(&this->m_fs);
    if (R_FAILED(rc))
        return;

    FsDir contentDir;
    std::strcpy(pathBuffer, amsContentsPath);
    rc = fsFsOpenDirectory(&this->m_fs, pathBuffer, FsDirOpenMode_ReadDirs, &contentDir);
    if (R_FAILED(rc))
        return;
    tsl::hlp::ScopeGuard dirGuard([&] { fsDirClose(&contentDir); });

    /* Iterate over contents folder. */
    for (const auto &entry : FsDirIterator(contentDir)) {
        FsFile toolboxFile;
        std::snprintf(pathBuffer, FS_MAX_PATH, "/atmosphere/contents/%.*s/toolbox.json", FS_MAX_PATH - 35, entry.name);
        rc = fsFsOpenFile(&this->m_fs, pathBuffer, FsOpenMode_Read, &toolboxFile);
        if (R_FAILED(rc))
            continue;
        tsl::hlp::ScopeGuard fileGuard([&] { fsFileClose(&toolboxFile); });

        /* Get toolbox file size. */
        s64 size;
        rc = fsFileGetSize(&toolboxFile, &size);
        if (R_FAILED(rc))
            continue;

        /* Read toolbox file. */
        std::string toolBoxData(size, '\0');
        u64 bytesRead;
        rc = fsFileRead(&toolboxFile, 0, toolBoxData.data(), size, FsReadOption_None, &bytesRead);
        if (R_FAILED(rc))
            continue;

        /* Parse toolbox file data. */
        json toolboxFileContent = json::parse(toolBoxData);

        const std::string &sysmoduleProgramIdString = toolboxFileContent["tid"];
        u64 sysmoduleProgramId = std::strtoul(sysmoduleProgramIdString.c_str(), nullptr, 16);

        /* Let's not allow Tesla to be killed with this. */
        if (sysmoduleProgramId == 0x420000000007E51AULL)
            continue;

        SystemModule module = {
            .listItem = new tsl::elm::ListItem(toolboxFileContent["name"]),
            .programId = sysmoduleProgramId,
            .needReboot = toolboxFileContent["requires_reboot"],
        };

        module.listItem->setClickListener([this, module](u64 click) -> bool {
            /* if the folder "flags" does not exist, it will be created */
            std::snprintf(pathBuffer, FS_MAX_PATH, boot2FlagFolder, module.programId);
            fsFsCreateDirectory(&this->m_fs, pathBuffer);
            std::snprintf(pathBuffer, FS_MAX_PATH, boot2FlagFormat, module.programId);

            if (click & HidNpadButton_A && !module.needReboot) {
                if (this->isRunning(module)) {
                    /* Kill process. */
                    pmshellTerminateProgram(module.programId);

                    /* Remove boot2 flag file. */
                    if (this->hasFlag(module))
                        fsFsDeleteFile(&this->m_fs, pathBuffer);
                } else {
                    /* Start process. */
                    const NcmProgramLocation programLocation{
                        .program_id = module.programId,
                        .storageID = NcmStorageId_None,
                    };
                    u64 pid = 0;
                    pmshellLaunchProgram(0, &programLocation, &pid);

                    /* Create boot2 flag file. */
                    if (!this->hasFlag(module))
                        fsFsCreateFile(&this->m_fs, pathBuffer, 0, FsCreateOption(0));
                }
                return true;
            }

            if (click & HidNpadButton_Y) {
                if (this->hasFlag(module)) {
                    /* Remove boot2 flag file. */
                    fsFsDeleteFile(&this->m_fs, pathBuffer);
                } else {
                    /* Create boot2 flag file. */
                    fsFsCreateFile(&this->m_fs, pathBuffer, 0, FsCreateOption(0));
                }
                return true;
            }

            return false;
        });
        this->m_sysmoduleListItems.push_back(std::move(module));
    }
    this->m_scanned = true;
}

GuiMain::~GuiMain() {
    fsFsClose(&this->m_fs);
}

// Method to draw available RAM only
inline void drawMemoryWidget(auto renderer) {
    static char ramString[24];  // Buffer for RAM string
    static tsl::Color ramColor = {0,0,0,0};
    static u64 lastUpdateTick = 0;
    const u64 ticksPerSecond = armGetSystemTickFreq();

    // Get the current tick count
    u64 currentTick = armGetSystemTick();

    // Check if this is the first run or at least one second has passed since the last update
    if (lastUpdateTick == 0 || currentTick - lastUpdateTick >= ticksPerSecond) {
        // Update RAM information
        u64 RAM_Used_system_u, RAM_Total_system_u;
        svcGetSystemInfo(&RAM_Used_system_u, 1, INVALID_HANDLE, 2);
        svcGetSystemInfo(&RAM_Total_system_u, 0, INVALID_HANDLE, 2);

        // Calculate free RAM and store in the buffer
        float freeRamMB = (static_cast<float>(RAM_Total_system_u - RAM_Used_system_u) / (1024.0f * 1024.0f));
        snprintf(ramString, sizeof(ramString), "%.2f MB %s", freeRamMB, ult::FREE.c_str());

        if (freeRamMB >= 9.0f){
            ramColor = tsl::healthyRamTextColor; // Green: R=0, G=15, B=0
        } else if (freeRamMB >= 3.0f) {
            ramColor = tsl::neutralRamTextColor; // Orange-ish: R=15, G=10, B=0 → roughly RGB888: 255, 170, 0
        } else {
            ramColor = tsl::badRamTextColor; // Red: R=15, G=0, B=0
        }
        // Update the last update tick
        lastUpdateTick = currentTick;
    }

    // Draw separator line (if necessary)
    renderer->drawRect(245, 23, 1, 49, renderer->a(tsl::separatorColor));

    size_t y_offset = 55; // Adjusted y_offset for drawing

    // Draw free RAM string
    renderer->drawString(ramString, false, tsl::cfg::FramebufferWidth - tsl::gfx::calculateStringWidth(ramString, 20, true) - 22, y_offset, 20, renderer->a(ramColor));
}

tsl::elm::Element *GuiMain::createUI() {
    //tsl::elm::OverlayFrame *rootFrame = new tsl::elm::OverlayFrame("시스템 모듈", "1.3.3-ASAP");

    auto *rootFrame = new tsl::elm::HeaderOverlayFrame(97);
    rootFrame->setHeader(new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer *renderer, s32 x, s32 y, s32 w, s32 h) {
        renderer->drawString("시스템 모듈", false, 20, 50+2, 32, renderer->a(tsl::defaultOverlayColor));
        renderer->drawString("1.3.4-ASAP", false, 20, 50+23, 15, renderer->a(tsl::versionTextColor));

        drawMemoryWidget(renderer);
    }));

    if (this->m_sysmoduleListItems.size() == 0) {
        const char *description = this->m_scanned ? "찾지 못했습니다!" : "스캔 실패!";

        auto *warning = new tsl::elm::CustomDrawer([description](tsl::gfx::Renderer *renderer, s32 x, s32 y, s32 w, s32 h) {
            renderer->drawString("\uE150", false, 180, 250, 90, renderer->a(0xFFFF));
            renderer->drawString(description, false, 110, 340, 25, renderer->a(0xFFFF));
        });

        rootFrame->setContent(warning);
    } else {
        tsl::elm::List *sysmoduleList = new tsl::elm::List();
        sysmoduleList->addItem(new tsl::elm::CategoryHeader("동적 모듈  |  \uE0E0  전환  |  \uE0E3  자동시작", true));
        sysmoduleList->addItem(new tsl::elm::CustomDrawer([](tsl::gfx::Renderer *renderer, s32 x, s32 y, s32 w, s32 h) {
            renderer->drawString("\uE016  해당 시스모듈은 상시 전환 가능합니다.", false, x + 5, y + 20, 15, renderer->a(tsl::accentTextColor));
        }), 30);
        for (const auto &module : this->m_sysmoduleListItems) {
            if (!module.needReboot)
                sysmoduleList->addItem(module.listItem);
        }

        sysmoduleList->addItem(new tsl::elm::CategoryHeader("정적 모듈  |  \uE0E3  자동시작", true));
        sysmoduleList->addItem(new tsl::elm::CustomDrawer([](tsl::gfx::Renderer *renderer, s32 x, s32 y, s32 w, s32 h) {
            renderer->drawString("\uE016  정상 작동을 위해 재부팅이 필요합니다.", false, x + 5, y + 20, 15, renderer->a(tsl::accentTextColor));
        }), 30);
        for (const auto &module : this->m_sysmoduleListItems) {
            if (module.needReboot)
                sysmoduleList->addItem(module.listItem);
        }
        rootFrame->setContent(sysmoduleList);
    }

    return rootFrame;
}

void GuiMain::update() {
    static u32 counter = 0;

    if (counter++ % 20 != 0)
        return;

    for (const auto &module : this->m_sysmoduleListItems) {
        this->updateStatus(module);
    }
}

void GuiMain::updateStatus(const SystemModule &module) {
    bool running = this->isRunning(module);
    bool hasFlag = this->hasFlag(module);

    const char *desc = descriptions[running][hasFlag];
    module.listItem->setValue(desc);
}

bool GuiMain::hasFlag(const SystemModule &module) {
    FsFile flagFile;
    std::snprintf(pathBuffer, FS_MAX_PATH, boot2FlagFormat, module.programId);
    Result rc = fsFsOpenFile(&this->m_fs, pathBuffer, FsOpenMode_Read, &flagFile);
    if (R_SUCCEEDED(rc)) {
        fsFileClose(&flagFile);
        return true;
    } else {
        return false;
    }
}

bool GuiMain::isRunning(const SystemModule &module) {
    u64 pid = 0;
    if (R_FAILED(pmdmntGetProcessId(&pid, module.programId)))
        return false;

    return pid > 0;
}
