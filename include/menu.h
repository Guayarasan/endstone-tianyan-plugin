//
// Created by yuhang on 2025/11/6.
//

#ifndef TIANYAN_COMMANDS_H
#define TIANYAN_COMMANDS_H
#include "global.h"
#include <endstone/endstone.hpp>

#include "translate.hpp"

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

    //查找实体密度高区域
    void findHighDensityRegion(endstone::Player &player, int size = 20) const;


private:
    endstone::Plugin &plugin_;
    translate* tran_;
};


#endif //TIANYAN_COMMANDS_H