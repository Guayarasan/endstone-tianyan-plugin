//
// Created by yuhang on 2025/11/6.
//

#ifndef TIANYAN_COMMANDS_H
#define TIANYAN_COMMANDS_H
#include "global.h"
#include <endstone/endstone.hpp>

#include "translate.hpp"
#include <endstone_inventoryui/inventoryui.h>

class TianyanPlugin;
class translate;

class Menu{
public:
    explicit Menu(TianyanPlugin* tianyan, translate* tran);

    //日志展示菜单
    void showLogMenu(endstone::Player &player, const std::vector<TianyanCore::LogData>& logDatas, int page = 0);

    //tyback菜单
    void tybackMenu(const endstone::Player &sender) const;

    //tys 菜单
    void tysMenu(const endstone::Player &sender) const;

    //ty菜单
    void tyMenu(const endstone::Player &sender) const;

    //查看在线玩家物品栏函数
    void showOnlinePlayerBag(const endstone::CommandSender &sender, const endstone::Player& player) const;

    //可视化物品栏展示（使用 inventoryui），失败返回 false
    bool showPlayerInventoryUI(endstone::Player &sender, endstone::Player &target);

    //注入 inventoryui 服务
    void setInventoryUIService(std::shared_ptr<inventoryui::InventoryUI> service);

    //查找实体密度高区域
    void findHighDensityRegion(endstone::Player &player, int size = 20) const;


private:
    endstone::Plugin &plugin_;
    translate* tran_;
    std::shared_ptr<inventoryui::InventoryUI> inventory_ui_service_;
    std::shared_ptr<inventoryui::Menu> last_inventory_menu_;
};


#endif //TIANYAN_COMMANDS_H
