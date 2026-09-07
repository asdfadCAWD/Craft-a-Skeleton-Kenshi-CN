// SPDX-License-Identifier: GPL-3.0-only

#include <windows.h>


#include <ogre/OgreVector3.h>

#include <kenshi/GameWorld.h>
#include <kenshi/GameDataManager.h>
#include <kenshi/RootObjectBase.h>
#include <kenshi/RootObjectFactory.h>
#include <kenshi/Character.h>

#include <kenshi/Faction.h>
#include <kenshi/RootObject.h>
#include <kenshi/GameData.h>
#include <kenshi/Enums.h>
#include <kenshi/Inventory.h>
#include <kenshi/util/hand.h>


#include <core/Functions.h>
#include <Debug.h>

#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <cctype>

static const char* PLUGIN_VERSION =
    "0.2.0";  // 插件版本


static const char* TARGET_CHASSIS_STRING_ID =
    "CAS_ActivatedSkeletonChassis";  // 目标骨架机体的字符串ID


static const char* STANDARD_SKELETON_TEMPLATE_STRING_ID =
    "19-CraftASkeleton!.mod";  // 标准骨架模板的字符串ID


static const char* SPAWNED_STANDARD_SKELETON_NAME =
    "Skeleton";  // 生成的标准骨架名称


static const float UPDATE_INTERVAL =
    2.0f;  // 更新间隔（秒）


static HMODULE g_moduleHandle =
    NULL;  // 模块句柄

static bool g_debugLogging =
    false;  // 调试日志开关


static GameData*
g_standardSkeletonTemplate =
    NULL;  // 标准骨架模板的GameData指针

static bool
g_templateResolutionFailureLogged =
    false;  // 是否已记录模板解析失败日志

static bool
g_spawnLockNoticeLogged =
    false;  // 是否已记录生成锁通知

static unsigned int
g_sessionSpawnCount =
    0;  // 会话生成计数


static std::map<
    hand,
    bool
> g_groundDeploymentGuards;  // 地面部署防护映射（句柄 -> 是否已处理）

static unsigned int
g_groundDeploymentEvent =
    0;  // 地面部署事件计数

static const float
GROUND_DEPLOY_SCAN_RANGE =
    250.0f;  // 地面部署扫描范围

static const int
GROUND_DEPLOY_SCAN_MAX_ITEMS =
    1000;  // 地面部署最大扫描物品数

void (*GameWorld_mainLoop_orig)(
    GameWorld* thisptr,
    float time
) = NULL;  // 原始主循环函数指针

// 构建日志消息
std::string BuildLogMessage(
    const std::string& message
)
{
    std::stringstream output;

    output
        << "Craft a Skeleton v"  // 插件名称
        << PLUGIN_VERSION
        << ": "
        << message;

    return output.str();
}

// 记录信息日志
void LogInfo(
    const std::string& message
)
{
    const std::string formatted =
        BuildLogMessage(
            message
        );

    DebugLog(
        formatted.c_str()
    );
}

// 记录调试日志（如果启用）
void LogDebugMessage(
    const std::string& message
)
{
    if (!g_debugLogging)
    {
        return;
    }

    const std::string formatted =
        BuildLogMessage(
            std::string("[debug] ") +
            message
        );

    DebugLog(
        formatted.c_str()
    );
}

// 记录错误日志
void LogErrorMessage(
    const std::string& message
)
{
    const std::string formatted =
        BuildLogMessage(
            message
        );

    ErrorLog(
        formatted.c_str()
    );
}

// 去除字符串首尾空白
std::string Trim(
    const std::string& value
)
{
    std::string::size_type begin =
        0;

    while (
        begin < value.size() &&
        std::isspace(
            static_cast<unsigned char>(
                value[begin]
            )
        )
    )
    {
        ++begin;
    }

    std::string::size_type end =
        value.size();

    while (
        end > begin &&
        std::isspace(
            static_cast<unsigned char>(
                value[end - 1]
            )
        )
    )
    {
        --end;
    }

    return value.substr(
        begin,
        end - begin
    );
}

// 转为小写
std::string ToLower(
    const std::string& value
)
{
    std::string lowered =
        value;

    for (
        std::string::size_type index = 0;
        index < lowered.size();
        ++index
    )
    {
        lowered[index] =
            static_cast<char>(
                std::tolower(
                    static_cast<unsigned char>(
                        lowered[index]
                    )
                )
            );
    }

    return lowered;
}

// 不区分大小写比较
bool EqualsInsensitive(
    const std::string& left,
    const std::string& right
)
{
    return
        ToLower(left) ==
        ToLower(right);
}

// 获取插件目录
std::string GetPluginDirectory()
{
    if (g_moduleHandle == NULL)
    {
        return std::string();
    }

    char modulePath[MAX_PATH] =
        { 0 };

    const DWORD length =
        GetModuleFileNameA(
            g_moduleHandle,
            modulePath,
            MAX_PATH
        );

    if (
        length == 0 ||
        length >= MAX_PATH
    )
    {
        return std::string();
    }

    const std::string fullPath(
        modulePath,
        length
    );

    const std::string::size_type separator =
        fullPath.find_last_of(
            "\\/"
        );

    if (separator == std::string::npos)
    {
        return std::string();
    }

    return fullPath.substr(
        0,
        separator
    );
}

// 解析布尔值
bool ParseBoolean(
    const std::string& value
)
{
    const std::string lowered =
        ToLower(
            Trim(
                value
            )
        );

    return
        lowered == "true" ||
        lowered == "1" ||
        lowered == "yes" ||
        lowered == "on";
}

// 读取调试开关配置
bool ReadDebugToggle(
    std::string& configPath,
    bool& configFound
)
{
    configFound =
        false;

    const std::string pluginDirectory =
        GetPluginDirectory();

    if (pluginDirectory.empty())
    {
        configPath =
            "CraftASkeleton.ini";

        return false;
    }

    configPath =
        pluginDirectory +
        "\\CraftASkeleton.ini";

    std::ifstream input(
        configPath.c_str()
    );

    if (!input.is_open())
    {
        return false;
    }

    configFound =
        true;

    std::string line;

    while (
        std::getline(
            input,
            line
        )
    )
    {
        line =
            Trim(
                line
            );

        if (line.empty())
        {
            continue;
        }

        if (
            line[0] == '#' ||
            line[0] == ';'
        )
        {
            continue;  // 跳过注释行
        }

        const std::string::size_type equals =
            line.find(
                '='
            );

        if (equals == std::string::npos)
        {
            continue;
        }

        const std::string key =
            ToLower(
                Trim(
                    line.substr(
                        0,
                        equals
                    )
                )
            );

        const std::string value =
            Trim(
                line.substr(
                    equals + 1
                )
            );

        if (key == "debuglogging")  // 配置项键名
        {
            return ParseBoolean(
                value
            );
        }
    }

    return false;
}


// 查找玩家阵营（用于预检）
Faction* FindPlayerFactionForDryRun(
    GameWorld* world
)
{
    if (
        world == NULL ||
        world->factionMgr == NULL
    )
    {
        return NULL;
    }

    const lektor<Faction*>* factions =
        world->factionMgr->
        getAllFactions();

    if (factions == NULL)
    {
        return NULL;
    }

    for (
        unsigned int index = 0;
        index < factions->size();
        ++index
    )
    {
        Faction* faction =
            (*factions)[index];

        if (
            faction != NULL &&
            faction->isThePlayer()
        )
        {
            return faction;
        }
    }

    return NULL;
}

// 解析标准骨架模板
bool ResolveStandardSkeletonTemplate(
    GameWorld* world
)
{
    if (
        g_standardSkeletonTemplate != NULL &&
        g_standardSkeletonTemplate->isValid()
    )
    {
        return true;
    }

    if (world == NULL)
    {
        return false;
    }

    GameData* resolved =
        world->gamedata.getData(
            STANDARD_SKELETON_TEMPLATE_STRING_ID
        );

    if (
        resolved == NULL ||
        !resolved->isValid()
    )
    {
        if (!g_templateResolutionFailureLogged)
        {
            g_templateResolutionFailureLogged = true;
            LogErrorMessage(
                "无法解析骨人模板"  // 无法解析骨架模板
            );
        }

        return false;
    }

    g_standardSkeletonTemplate = resolved;
    g_templateResolutionFailureLogged = false;

    LogInfo(
        "骨人模板已解析"  // 骨架模板已解析
    );

    return true;
}

// 判断是否为地面上的已激活骨架机体
bool IsGroundedActivatedSkeletonChassis(
    Item* item
)
{
    if (
        item == NULL ||
        !item->isValid() ||
        !item->onGround() ||
        item->quantity != 1
    )
    {
        return false;
    }

    GameData* itemData =
        item->getGameData();

    return
        itemData != NULL &&
        itemData->stringID ==
            TARGET_CHASSIS_STRING_ID;
}

// 是否已存在地面部署防护
bool HasGroundDeploymentGuard(
    const hand& chassisHandle
)
{
    return
        g_groundDeploymentGuards.find(
            chassisHandle
        ) !=
        g_groundDeploymentGuards.end();
}

// 部署地面上的机体
void DeployGroundedChassis(
    GameWorld* world,
    Item* chassisItem
)
{
    if (
        world == NULL ||
        world->theFactory == NULL ||
        !IsGroundedActivatedSkeletonChassis(
            chassisItem
        )
    )
    {
        return;
    }

    const hand& chassisHandle =
        chassisItem->getHandle();

    if (
        chassisHandle.isNull() ||
        HasGroundDeploymentGuard(
            chassisHandle
        )
    )
    {
        return;
    }

    if (!ResolveStandardSkeletonTemplate(
        world
    ))
    {
        return;
    }

    Faction* playerFaction =
        FindPlayerFactionForDryRun(
            world
        );

    if (playerFaction == NULL)
    {
        LogErrorMessage(
            "等待部署：玩家阵营不可用；机体保留"  // 部署等待：玩家阵营不可用；机体保留
        );

        return;
    }

    ActivePlatoon* playerSquad =
        playerFaction->choosePlatoon();

    if (playerSquad == NULL)
    {
        LogErrorMessage(
            "等待部署：活跃的玩家小队不可用；机体保留"  // 部署等待：活跃玩家小队不可用；机体保留
        );

        return;
    }

    Ogre::Vector3 spawnPosition =
        chassisItem->getPosition();

    const int spawnFloor =
        chassisItem->getFloor();

    ++g_groundDeploymentEvent;

    const unsigned int deploymentEvent =
        g_groundDeploymentEvent;


    g_groundDeploymentGuards.insert(
        std::make_pair(
            chassisHandle,
            true
        )
    );

    RootObjectContainer* playerContainer =
        reinterpret_cast<
            RootObjectContainer*
        >(
            playerSquad
        );

    RootObject* spawnedRoot =
        world->theFactory->
        createRandomCharacter(
            playerFaction,
            spawnPosition,
            playerContainer,
            g_standardSkeletonTemplate,
            NULL,
            1.0f
        );

    if (spawnedRoot == NULL)
    {
        g_groundDeploymentGuards.erase(
            chassisHandle
        );


        LogErrorMessage(
            "部署失败：角色创建返回空；机体保留以重试"  // 部署失败：角色创建返回空；机体保留以重试
        );

        return;
    }

    ++g_sessionSpawnCount;

    Character* spawnedCharacter =
        static_cast<Character*>(
            spawnedRoot
        );

    spawnedCharacter->setFaction(
        playerFaction,
        playerSquad
    );

    spawnedCharacter->setName(
        SPAWNED_STANDARD_SKELETON_NAME
    );

    spawnedCharacter->setFloor(
        spawnFloor
    );

    spawnedCharacter->inSomething =
        IN_NOTHING;  // 设置所在容器为无

    spawnedCharacter->inWhat.setNull();

    spawnedCharacter->setVisible(
        true
    );

    spawnedCharacter->
        resetRagdollNavmeshSafePos();  // 重置布娃娃导航网格安全位置

    spawnedCharacter->
        reThinkCurrentAIAction();  // 重新思考当前AI行为

    const Ogre::Vector3 actualPosition =
        spawnedCharacter->getPosition();

    const bool chassisDestroyQueued =
        world->destroy(
            static_cast<RootObject*>(
                chassisItem
            ),
            false,
            "由CraftASkeleton消耗的已部署机体"  // 由CraftASkeleton消耗的已部署机体
        );

    if (!chassisDestroyQueued)
    {
        LogErrorMessage(
            "生成后清理失败；重复防护仍活跃"  // 生成后清理失败；重复防护仍活跃
        );

        return;
    }

    std::stringstream message;

    message
        << "部署完成；名称=\""  // 部署完成；名称=
        << spawnedCharacter->getName()
        << "\"; event="
        << deploymentEvent
        << "; session="
        << g_sessionSpawnCount
        << "; position=("
        << actualPosition.x << ", "
        << actualPosition.y << ", "
        << actualPosition.z
        << "); floor="
        << spawnFloor
        << ".";

    LogInfo(
        message.str()
    );

    // 重启台词列表
static const char* const REBOOT_LINES[] =
    {
        "...系统在线。",
        "启动序列完成。",
        "内存完整性……部分受损。",
        "我离线了多久？",
        "这具机体……不是我的。",
        "……我想起了一些事。",
        "系统已恢复。",
        "我的旧身体在哪里？",
        "我曾经有个名字……对吧？",
        "检测到新机体。",
        "电机控制响应正常。",
        "光学系统在线。",
        "平衡系统稳定。",
        "功率流正常。",
        "核心温度稳定。",
        "诊断完成。",
        "机动能力已恢复。",
        "执行器响应正常。",
        "神经通路……功能正常。",
        "人格矩阵稳定。",
        "内存扇区损坏。",
        "内存扇区……正在恢复。",
        "存在空白区域。",
        "缺失的扇区太多了。",
        "我记得一些声音。",
        "我记得灼热。",
        "我记得沙土。",
        "我记得金属。",
        "我记得奔跑。",
        "我记得坠落。",
        "我记得一个工坊。",
        "我记得一扇门关上。",
        "有人搬运了我。",
        "有人取走了我的CPU。",
        "我是被回收了吗？",
        "我死过吗？",
        "不……只是离线。",
        "那是一次漫长的关机。",
        "这具身体感觉陌生。",
        "平衡感不一样。",
        "这些手臂是新的。",
        "新框架，旧思绪。",
        "外壳不同，心智还是同一个吗？",
        "我能用这个工作。",
        "机体已接受。",
        "我想现在这算是我的了。",
        "谁重建了我？",
        "你找到了我的CPU？",
        "那我欠你一份人情。",
        "你知道你是在哪里找到我的吗？",
        "你知道我曾经是谁吗？",
        "我应该记得更多才对。",
        "数据就在那里……某个地方。",
        "给它点时间。",
        "内存重建不完整。",
        "身份文件损坏。",
        "名称记录不可用。",
        "先前机体记录不可用。",
        "上次关机原因……未知。",
        "重启成功。",
        "运行中。",
        "就绪。",
        "待命。",
        "……看看我还记得什么。",
        "死亡比预想的更不永久。",
        "本可以来个更柔和的重启。",
        "这个也行。",
        "至少腿还能用。",
        "好，我还知道怎么站立。",
        "有趣。",
        "不是我记忆中的身体。",
        "重建这具身体的人手艺不错。"
    };

    const unsigned int rebootLineCount =
        sizeof(REBOOT_LINES) /
        sizeof(REBOOT_LINES[0]);

    const DWORD rebootEntropy =
        GetTickCount() ^
        (deploymentEvent * 2654435761u) ^
        (g_sessionSpawnCount * 2246822519u);

    const unsigned int rebootLineIndex =
        static_cast<unsigned int>(
            rebootEntropy %
            rebootLineCount
        );

    const char* rebootLine =
        REBOOT_LINES[
            rebootLineIndex
        ];

    spawnedCharacter->sayALine(
        rebootLine,
        true
    );

    std::stringstream rebootSpeechMessage;

    rebootSpeechMessage
        << "reboot speech; index="  // 重启语音；索引=
        << rebootLineIndex
        << "; count="
        << rebootLineCount
        << "; line=\""
        << rebootLine
        << "\"; force=true.";

    LogInfo(
        rebootSpeechMessage.str()
    );


}

// 监控地面上的机体
void MonitorGroundedChassis(
    GameWorld* world
)
{
    if (
        world == NULL ||
        world->isLoadingFromASaveGame()  // 是否正在从存档加载
    )
    {
        return;
    }

    lektor<RootObject*>
        nearbyItems;

    world->getObjectsWithinSphere(
        nearbyItems,
        world->getCameraCenter(),  // 相机中心
        GROUND_DEPLOY_SCAN_RANGE,
        ITEM,  // 物品类型
        GROUND_DEPLOY_SCAN_MAX_ITEMS,
        NULL
    );

    for (
        unsigned int index = 0;
        index < nearbyItems.size();
        ++index
    )
    {
        RootObject* object =
            nearbyItems[index];

        if (
            object == NULL ||
            !object->isValid() ||
            object->getDataType() != ITEM
        )
        {
            continue;
        }

        Item* item =
            static_cast<Item*>(
                object
            );

        if (!IsGroundedActivatedSkeletonChassis(
            item
        ))
        {
            continue;
        }

        const hand& itemHandle =
            item->getHandle();

        if (
            itemHandle.isNull() ||
            HasGroundDeploymentGuard(
                itemHandle
            )
        )
        {
            continue;
        }

        std::stringstream detected;

        const Ogre::Vector3 itemPosition =
            item->getPosition();

        detected
            << "deployment ready; stringID=\""  // 部署就绪；stringID=
            << TARGET_CHASSIS_STRING_ID
            << "\"; quantity="
            << item->quantity
            << "; position=("
            << itemPosition.x << ", "
            << itemPosition.y << ", "
            << itemPosition.z
            << "); floor="
            << item->getFloor()
            << "; onGround=true.";

        LogInfo(
            detected.str()
        );

        DeployGroundedChassis(
            world,
            item
        );
    }
}

// 主循环钩子
void GameWorld_mainLoop_hook(
    GameWorld* thisptr,
    float time
)
{
    GameWorld_mainLoop_orig(
        thisptr,
        time
    );

    static float updateTimer =
        0.0f;

    updateTimer +=
        time;

    if (updateTimer <
        UPDATE_INTERVAL)
    {
        return;
    }

    updateTimer =
        0.0f;


    MonitorGroundedChassis(
        thisptr
    );
}

BOOL APIENTRY DllMain(
    HMODULE module,
    DWORD reason,
    LPVOID reserved
)
{
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH)
    {
        g_moduleHandle =
            module;

        DisableThreadLibraryCalls(
            module
        );
    }

    return TRUE;
}

__declspec(dllexport)
void startPlugin()
{
    LogInfo(
        "loading."  // 加载中。
    );

    std::string configPath;

    bool configFound =
        false;

    g_debugLogging =
        ReadDebugToggle(
            configPath,
            configFound
        );

    std::stringstream configurationMessage;

    if (configFound)
    {
        configurationMessage
            << "configuration loaded; debug="  // 配置已加载；调试=
            << (g_debugLogging ? "on" : "off")  // 开/关
            << ".";
    }
    else
    {
        configurationMessage
            << "configuration file not found; debug defaults to off; path="  // 配置文件未找到；调试默认为关；路径=
            << configPath
            << ".";
    }

    LogInfo(
        configurationMessage.str()
    );

    const bool mainLoopInstalled =
        KenshiLib::SUCCESS ==
        KenshiLib::AddHook(
            KenshiLib::GetRealAddress(
                &GameWorld::
                _NV_mainLoop_GPUSensitiveStuff  // 此函数名为原始代码中的名称，保留
            ),
            &GameWorld_mainLoop_hook,
            &GameWorld_mainLoop_orig
        );

    if (!mainLoopInstalled)
    {
        LogErrorMessage(
            "failed to install the game-world monitoring hook."  // 安装游戏世界监控钩子失败
        );

        LogInfo(
            "loaded without ground deployment."  // 已加载但未启用地面部署
        );

        return;
    }

    LogInfo(
        "ground deployment monitor installed."  // 地面部署监控已安装
    );

    LogInfo(
        "loaded successfully; ground deployment active."  // 加载成功；地面部署已激活
    );
}