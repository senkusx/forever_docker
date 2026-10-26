/*
 * Companions: the bot brain, items. Trading with its players (it takes what they hand over and gives what they ask
 * for), looting its corpses for what it can use, potions and healthstones when low.
 */

#include "CompanionAI.h"
#include "CellImpl.h"
#include "CompanionConfig.h"
#include "CompanionFactory.h"
#include "CompanionMgr.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Bag.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Loot.h"
#include "LootPackets.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TradeData.h"
#include "TradePackets.h"
#include "WorldSession.h"

namespace Companions
{
    namespace
    {
        std::string Lower(std::string text)
        {
            for (char& c : text)
                c = char(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        std::string ItemName(ItemTemplate const* proto)
        {
            return Lower(proto->GetName(LOCALE_enUS));
        }

        // every item in its backpack and bags, with where it is
        struct BagItem { Item* Value; uint8 Bag; uint8 Slot; };
        std::vector<BagItem> BagItems(Player* bot)
        {
            std::vector<BagItem> items;
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    items.push_back({ item, INVENTORY_SLOT_BAG_0, slot });
            for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
                if (Bag* container = bot->GetBagByPos(bag))
                    for (uint32 slot = 0; slot < container->GetBagSize(); ++slot)
                        if (Item* item = container->GetItemByPos(uint8(slot)))
                            items.push_back({ item, bag, uint8(slot) });
            return items;
        }

        // the spell an item casts when used, with an effect of that kind (healing, mana)
        uint32 UseSpell(ItemTemplate const* proto, SpellEffects effectName, Powers power = POWER_HEALTH)
        {
            for (ItemEffectEntry const* effect : proto->Effects)
            {
                if (!effect || effect->TriggerType != ITEM_SPELLTRIGGER_ON_USE || effect->SpellID <= 0)
                    continue;
                SpellInfo const* info = sSpellMgr->GetSpellInfo(effect->SpellID, DIFFICULTY_NONE);
                if (!info)
                    continue;
                for (SpellEffectInfo const& spellEffect : info->GetEffects())
                    if (spellEffect.IsEffect(effectName) && (effectName != SPELL_EFFECT_ENERGIZE || spellEffect.MiscValue == power))
                        return info->Id;
            }
            return 0;
        }

        // "potions", "food", "drink", "bandages", "junk", "cloth"... or a name
        bool Matches(ItemTemplate const* proto, std::string const& what)
        {
            uint32 const itemClass = proto->GetClass();
            uint32 const subClass = proto->GetSubClass();
            if (what == "potions" || what == "potion" || what == "pots")
                return itemClass == ITEM_CLASS_CONSUMABLE && (subClass == ITEM_SUBCLASS_POTION || subClass == ITEM_SUBCLASS_ELIXIR
                    || subClass == ITEM_SUBCLASS_FLASK);
            if (what == "food" || what == "drink" || what == "drinks" || what == "water")
                return itemClass == ITEM_CLASS_CONSUMABLE && subClass == ITEM_SUBCLASS_FOOD_DRINK;
            if (what == "bandages" || what == "bandage")
                return itemClass == ITEM_CLASS_CONSUMABLE && subClass == ITEM_SUBCLASS_BANDAGE;
            if (what == "junk" || what == "greys" || what == "grays" || what == "trash")
                return proto->GetQuality() == ITEM_QUALITY_POOR;
            if (what == "cloth")
                return itemClass == ITEM_CLASS_TRADE_GOODS && subClass == ITEM_SUBCLASS_CLOTH;
            if (what == "consumables")
                return itemClass == ITEM_CLASS_CONSUMABLE;
            if (what == "everything" || what == "all" || what == "stuff")
                return true;
            return !what.empty() && ItemName(proto).find(what) != std::string::npos;
        }
    }

    // ------------------------------------------------------------------ trading

    void CompanionAI::UpdateTrade()
    {
        TradeData* trade = _bot->GetTradeData();
        WorldSession* session = _bot->GetSession();
        if (!trade)
        {
            // over: thanks for what came with it
            if (!_tradeDone.IsEmpty())
                OnTradeFinished();
            _tradeBegun.Clear();
            // the trade it asked for was refused or never answered
            if (!_giveTo.IsEmpty() && GameTime::Now() > _giveUntil)
                _giveTo.Clear();
            return;
        }

        Player* trader = trade->GetTrader();
        bool const mine = trader && !_ambient && (trader->GetGUID() == _owner
            || (_bot->GetGroup() && _bot->GetGroup()->IsMember(trader->GetGUID()) && !sCompanionMgr->IsBotGuid(trader->GetGUID())));
        if (!mine)
        {
            WorldPackets::Trade::CancelTrade cancel{ WorldPacket(CMSG_CANCEL_TRADE) };
            session->HandleCancelTradeOpcode(cancel);
            if (trader && !sCompanionMgr->IsBotGuid(trader->GetGUID()))
                _bot->Whisper(_ambient ? "Not trading with strangers, sorry. Invite me to your group?" : "I only trade with my group.",
                    LANG_UNIVERSAL, trader);
            return;
        }

        TradeData* theirs = trader->GetTradeData();
        if (!theirs)
            return;

        // their trade: the bot opens the window (what a player's click on "accept" to a trade request does)
        if (_giveTo != trader->GetGUID() && !theirs->IsAccepted() && !trade->IsAccepted() && _tradeBegun != trader->GetGUID())
        {
            _tradeBegun = trader->GetGUID();
            WorldPackets::Trade::BeginTrade begin{ WorldPacket(CMSG_BEGIN_TRADE) };
            session->HandleBeginTradeOpcode(begin);
            ScanBags();     // what it has now: whatever is new afterwards came with the trade
            _lastNewItem.Clear();
        }

        // its trade: what it was asked for, once
        if (_giveTo == trader->GetGUID() && !_giveFilled)
        {
            _giveFilled = true;
            uint8 tradeSlot = 0;
            for (BagItem const& entry : BagItems(_bot))
            {
                if (tradeSlot >= TRADE_SLOT_TRADED_COUNT)
                    break;
                Item* item = entry.Value;
                ItemTemplate const* proto = item->GetTemplate();
                if (!proto || !Matches(proto, _giveWhat) || item->IsSoulBound() || !item->CanBeTraded(false, true))
                    continue;
                WorldPackets::Trade::SetTradeItem put{ WorldPacket(CMSG_SET_TRADE_ITEM) };
                put.TradeSlot = tradeSlot++;
                put.PackSlot = entry.Bag;
                put.ItemSlotInPack = entry.Slot;
                session->HandleSetTradeItemOpcode(put);
            }
        }

        // the player accepted: so does the bot (a change on their side unaccepts both, and it accepts again)
        if (theirs->IsAccepted() && !trade->IsAccepted())
        {
            WorldPackets::Trade::AcceptTrade accept{ WorldPacket(CMSG_ACCEPT_TRADE) };
            accept.StateIndex = theirs->GetServerStateIndex();
            session->HandleAcceptTradeOpcode(accept);
            _tradeDone = trader->GetGUID();
        }
    }

    void CompanionAI::OnTradeFinished()
    {
        ObjectGuid const with = _tradeDone;
        _tradeDone.Clear();
        _tradeBegun.Clear();
        _giveTo.Clear();
        Player* trader = ObjectAccessor::FindConnectedPlayer(with);
        Item* received = ScanBags();
        if (!received)
            return;
        ItemTemplate const* proto = received->GetTemplate();
        std::string name = proto ? proto->GetName(LOCALE_enUS) : "that";
        std::string line;
        if (proto && proto->GetInventoryType() != INVTYPE_NON_EQUIP)
            line = sCompanionFactory->IsUpgrade(_bot, proto, GetEffectiveRole())
                ? "Thanks for " + name + "! Say \"equip it\" and I'll put it on."
                : "Thanks for " + name + ". I'll keep it in my bags.";
        else
            line = "Thanks, " + name + " will come in handy.";
        if (trader)
            _bot->Whisper(line, LANG_UNIVERSAL, trader);
    }

    bool CompanionAI::OrderGive(Player* to, std::string const& what, std::string& reply)
    {
        std::string wanted = Lower(what);
        // "it", "that": what it got last
        if ((wanted == "it" || wanted == "that") && !_lastNewItem.IsEmpty())
            if (Item* last = _bot->GetItemByGuid(_lastNewItem))
                wanted = ItemName(last->GetTemplate());
        uint32 found = 0;
        bool bound = false;
        for (BagItem const& entry : BagItems(_bot))
        {
            ItemTemplate const* proto = entry.Value->GetTemplate();
            if (!proto || !Matches(proto, wanted))
                continue;
            if (entry.Value->IsSoulBound() || !entry.Value->CanBeTraded(false, true))
                bound = true;
            else
                ++found;
        }
        if (!found)
        {
            reply = bound ? "That's soulbound, I can't hand it over." : "I don't have any " + what + ".";
            return false;
        }
        if (_bot->GetTradeData() || to->GetTradeData())
        {
            reply = "Finish the other trade first.";
            return false;
        }
        if (!_bot->IsWithinDistInMap(to, 10.0f))
        {
            _bot->GetMotionMaster()->MoveFollow(to, 2.0f);
            reply = "Coming over, ask me again when I'm next to you.";
            return false;
        }
        _giveTo = to->GetGUID();
        _giveWhat = wanted;
        _giveFilled = false;
        _giveUntil = GameTime::Now() + Seconds(60);
        WorldPackets::Trade::InitiateTrade open{ WorldPacket(CMSG_INITIATE_TRADE) };
        open.Guid = to->GetGUID();
        _bot->GetSession()->HandleInitiateTradeOpcode(open);
        if (!_bot->GetTradeData())
        {
            _giveTo.Clear();
            reply = "I can't trade with you right now.";
            return false;
        }
        reply = "Here you go, accept the trade.";
        return true;
    }

    // ------------------------------------------------------------------ potions

    bool CompanionAI::UsePotion()
    {
        if (!_usePotions || GameTime::Now() < _potionAt)
            return false;
        bool const lowHealth = HealthPct() < 30.0f;
        bool const lowMana = _bot->GetMaxPower(POWER_MANA) > 0 && ManaPct() < 15.0f;
        if (!lowHealth && !lowMana)
            return false;
        _potionAt = GameTime::Now() + Seconds(2);
        for (BagItem const& entry : BagItems(_bot))
        {
            ItemTemplate const* proto = entry.Value->GetTemplate();
            if (!proto || proto->GetClass() != ITEM_CLASS_CONSUMABLE || proto->GetBaseRequiredLevel() > int32(_bot->GetLevel()))
                continue;
            uint32 spellId = 0;
            if (lowHealth)
                spellId = UseSpell(proto, SPELL_EFFECT_HEAL);
            if (!spellId && lowMana)
                spellId = UseSpell(proto, SPELL_EFFECT_ENERGIZE, POWER_MANA);
            if (!spellId)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
            if (!info || !_bot->GetSpellHistory()->IsReady(info, proto->GetId()))
                continue;
            CastSpellExtraArgs args(TRIGGERED_NONE);
            args.SetCastItem(entry.Value);
            if (_bot->CastSpell(_bot, spellId, args) == SPELL_CAST_OK)
            {
                _recentCasts.emplace_back(GameTime::Now(), ItemName(proto));
                return true;
            }
        }
        return false;
    }

    // ------------------------------------------------------------------ looting

    bool CompanionAI::UpdateLooting()
    {
        if (_lootMode == 2 || !_view.Enemies.empty() || _bot->IsInCombat() || _bot->GetTradeData())
            return false;
        TimePoint const now = GameTime::Now();

        if (!_lootCorpse.IsEmpty())
        {
            Creature* corpse = ObjectAccessor::GetCreature(*_bot, _lootCorpse);
            Loot* loot = corpse ? corpse->GetLootForPlayer(_bot) : nullptr;
            if (!corpse || corpse->IsAlive() || now > _lootUntil || !loot || loot->isLooted() || !_bot->isAllowedToLoot(corpse))
            {
                if (corpse)
                    _looted.push_back(_lootCorpse);
                _lootCorpse.Clear();
                return false;
            }
            if (_bot->GetDistance(corpse) > 4.0f)
            {
                if (!_bot->isMoving())
                {
                    _bot->GetMotionMaster()->MovePoint(0, corpse->GetPosition());
                    _movement = MoveMode::Point;
                }
                return true;
            }
            LootCorpse(corpse);
            _looted.push_back(_lootCorpse);
            _lootCorpse.Clear();
            return true;
        }

        if (now < _lootScanAt)
            return false;
        _lootScanAt = now + Seconds(2);
        if (_looted.size() > 50)
            _looted.erase(_looted.begin(), _looted.begin() + 25);

        // a companion stays with its owner: only corpses close to both, and none the owner is about to loot himself
        Player* owner = _view.Owner;
        std::list<Creature*> corpses;
        Trinity::AllWorldObjectsInRange check(_bot, 25.0f);
        Trinity::CreatureListSearcher<Trinity::AllWorldObjectsInRange> searcher(_bot, corpses, check);
        Cell::VisitGridObjects(_bot, searcher, 25.0f);
        Creature* best = nullptr;
        for (Creature* corpse : corpses)
        {
            if (corpse->IsAlive() || std::find(_looted.begin(), _looted.end(), corpse->GetGUID()) != _looted.end())
                continue;
            if (owner && (owner->GetDistance(corpse) > 30.0f || owner->GetDistance(corpse) < 4.0f))
                continue;
            if (!_bot->isAllowedToLoot(corpse))
                continue;
            Loot* loot = corpse->GetLootForPlayer(_bot);
            if (!loot || loot->isLooted() || !WantsSomething(loot))
                continue;
            if (!best || _bot->GetDistance(corpse) < _bot->GetDistance(best))
                best = corpse;
        }
        if (!best)
            return false;
        _lootCorpse = best->GetGUID();
        _lootUntil = now + Seconds(10);
        return false;
    }

    bool CompanionAI::WantsLoot(ItemTemplate const* proto) const
    {
        if (!proto)
            return false;
        if (_lootMode == 1)
            return true;
        // what it can use: food, drink, potions, bandages at its level, ammunition for its bow or gun, gear it would wear
        uint32 const itemClass = proto->GetClass();
        if (itemClass == ITEM_CLASS_CONSUMABLE)
            return proto->GetBaseRequiredLevel() <= int32(_bot->GetLevel()) && proto->GetSubClass() != ITEM_SUBCLASS_CONSUMABLE_OTHER;
        if (itemClass == ITEM_CLASS_PROJECTILE)
            return _bot->GetClass() == CLASS_HUNTER;
        if (itemClass == ITEM_CLASS_WEAPON || itemClass == ITEM_CLASS_ARMOR)
            return sCompanionFactory->IsUpgrade(_bot, proto, GetEffectiveRole());
        return false;
    }

    bool CompanionAI::WantsSomething(Loot const* loot) const
    {
        if (loot->gold)
            return true;
        for (LootItem const& item : loot->items)
            if (!item.is_looted && !item.is_blocked && item.type == LootItemType::Item && item.AllowedForPlayer(_bot, loot)
                && WantsLoot(sObjectMgr->GetItemTemplate(item.itemid)))
                return true;
        return false;
    }

    // what a player's client does: open it, take the coins and what it wants, close it (the rest stays for the others)
    void CompanionAI::LootCorpse(Creature* corpse)
    {
        WorldSession* session = _bot->GetSession();
        WorldPackets::Loot::LootUnit open{ WorldPacket(CMSG_LOOT_UNIT) };
        open.Unit = corpse->GetGUID();
        session->HandleLootOpcode(open);

        if (Loot* loot = corpse->GetLootForPlayer(_bot))
        {
            if (loot->gold)
            {
                WorldPackets::Loot::LootMoney money{ WorldPacket(CMSG_LOOT_MONEY) };
                session->HandleLootMoneyOpcode(money);
            }
            std::vector<uint32> wanted;
            for (LootItem const& item : loot->items)
                if (!item.is_looted && !item.is_blocked && item.type == LootItemType::Item && item.AllowedForPlayer(_bot, loot)
                    && WantsLoot(sObjectMgr->GetItemTemplate(item.itemid)))
                    wanted.push_back(item.LootListId);
            for (uint32 listId : wanted)
            {
                WorldPackets::Loot::LootItem take{ WorldPacket(CMSG_LOOT_ITEM) };
                take.Loot.push_back({ loot->GetGUID(), uint8(listId) });
                session->HandleAutostoreLootItemOpcode(take);
            }
        }

        WorldPackets::Loot::LootRelease close{ WorldPacket(CMSG_LOOT_RELEASE) };
        close.Unit = corpse->GetGUID();
        session->HandleLootReleaseOpcode(close);
        if (Item* item = ScanBags())
            if (ItemTemplate const* proto = item->GetTemplate(); proto && sCompanionFactory->IsUpgrade(_bot, proto, GetEffectiveRole()))
                CompanionFactory::EquipFromBags(_bot, item);    // its own loot: an upgrade goes on at once
    }
}
