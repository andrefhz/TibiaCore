/**
 * Tibia GIMUD Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2017  Alejandro Mujica <alejandrodemujica@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "otpch.h"

#include "pugicast.h"

#include "house.h"
#include "iologindata.h"
#include "game.h"
#include "configmanager.h"
#include "bed.h"

extern ConfigManager g_config;
extern Game g_game;

House::House(uint32_t houseId) : id(houseId) {}

void House::addTile(HouseTile* tile)
{
	tile->setFlag(TILESTATE_PROTECTIONZONE);
	houseTiles.push_back(tile);
}

void House::setOwner(uint32_t guid, bool updateDatabase/* = true*/, Player* player/* = nullptr*/)
{
	if (updateDatabase && owner != guid) {
		Database* db = Database::getInstance();

		std::ostringstream query;
		query << "UPDATE `houses` SET `owner` = " << guid << ", `bid` = 0, `bid_end` = 0, `last_bid` = 0, `highest_bidder` = 0  WHERE `id` = " << id;
		db->executeQuery(query.str());
	}

	if (isLoaded && owner == guid) {
		return;
	}

	isLoaded = true;

	if (owner != 0) {
		//send items to depot
		if (player) {
			transferToDepot(player);
		} else {
			transferToDepot();
		}

		for (HouseTile* tile : houseTiles) {
			if (const CreatureVector* creatures = tile->getCreatures()) {
				for (int32_t i = creatures->size(); --i >= 0;) {
					kickPlayer(nullptr, (*creatures)[i]->getPlayer());
				}
			}
		}

		// Remove players from beds
		for (BedItem* bed : bedsList) {
			if (bed->getSleeper() != 0) {
				bed->wakeUp(nullptr);
			}
		}

		//clean access lists
		owner = 0;
		setAccessList(SUBOWNER_LIST, "");
		setAccessList(GUEST_LIST, "");

		for (Door* door : doorList) {
			door->setAccessList("");
		}

		//reset paid date
		paidUntil = 0;
		rentWarnings = 0;
	}

	if (guid != 0) {
		std::string name = IOLoginData::getNameByGuid(guid);
		if (!name.empty()) {
			owner = guid;
			ownerName = name;
		}
	}

	updateDoorDescription();
}

void House::updateDoorDescription() const
{
	std::ostringstream ss;
	if (owner != 0) {
		ss << "It belongs to house '" << houseName << "'. " << ownerName << " owns this house.";
	} else {
		ss << "It belongs to house '" << houseName << "'. Nobody owns this house.";

		/*const int32_t housePrice = getRent();
		if (housePrice != -1) {
			ss << " It costs " << housePrice * g_config.getNumber(ConfigManager::HOUSE_PRICE) << " gold coins.";
		}*/
	}

	for (const auto& it : doorList) {
		it->setSpecialDescription(ss.str());
	}
}

AccessHouseLevel_t House::getHouseAccessLevel(const Player* player)
{
	if (!player) {
		return HOUSE_OWNER;
	}

	if (player->hasFlag(PlayerFlag_CanEditHouses)) {
		return HOUSE_OWNER;
	}

	if (player->getGUID() == owner) {
		return HOUSE_OWNER;
	}

	if (subOwnerList.isInList(player)) {
		return HOUSE_SUBOWNER;
	}

	if (guestList.isInList(player)) {
		return HOUSE_GUEST;
	}

	return HOUSE_NOT_INVITED;
}

bool House::kickPlayer(Player* player, Player* target)
{
	if (!target) {
		return false;
	}

	HouseTile* houseTile = dynamic_cast<HouseTile*>(target->getTile());
	if (!houseTile || houseTile->getHouse() != this) {
		return false;
	}

	if (getHouseAccessLevel(player) < getHouseAccessLevel(target) || target->hasFlag(PlayerFlag_CanEditHouses)) {
		return false;
	}

	Position oldPosition = target->getPosition();
	if (g_game.internalTeleport(target, getEntryPosition()) == RETURNVALUE_NOERROR) {
		g_game.addMagicEffect(oldPosition, CONST_ME_POFF);
		g_game.addMagicEffect(getEntryPosition(), CONST_ME_TELEPORT);
	}
	return true;
}

void House::setAccessList(uint32_t listId, const std::string& textlist)
{
	if (listId == GUEST_LIST) {
		guestList.parseList(textlist);
	} else if (listId == SUBOWNER_LIST) {
		subOwnerList.parseList(textlist);
	} else {
		Door* door = getDoorByNumber(listId);
		if (door) {
			door->setAccessList(textlist);
		}

		// We dont have kick anyone
		return;
	}

	//kick uninvited players
	for (HouseTile* tile : houseTiles) {
		if (CreatureVector* creatures = tile->getCreatures()) {
			for (int32_t i = creatures->size(); --i >= 0;) {
				Player* player = (*creatures)[i]->getPlayer();
				if (player && !isInvited(player)) {
					kickPlayer(nullptr, player);
				}
			}
		}
	}
}

bool House::transferToDepot() const
{
	if (townId == 0 || owner == 0) {
		return false;
	}

	Player* player = g_game.getPlayerByGUID(owner);
	if (player) {
		transferToDepot(player);
	} else {
		Player tmpPlayer(nullptr);
		if (!IOLoginData::loadPlayerById(&tmpPlayer, owner)) {
			return false;
		}

		transferToDepot(&tmpPlayer);
		IOLoginData::savePlayer(&tmpPlayer);
	}

	return true;
}

bool House::transferToDepot(Player* player) const
{
	if (townId == 0 || owner == 0) {
		return false;
	}

	ItemList moveItemList;
	for (HouseTile* tile : houseTiles) {
		if (const TileItemVector* items = tile->getItemList()) {
			for (Item* item : *items) {
				if (item->isPickupable()) {
					moveItemList.push_back(item);
				} else {
					Container* container = item->getContainer();
					if (container) {
						for (Item* containerItem : container->getItemList()) {
							moveItemList.push_back(containerItem);
						}
					}
				}
			}
		}
	}

	for (Item* item : moveItemList) {
		g_game.internalMoveItem(item->getParent(), player->getDepotLocker(getTownId(), true), INDEX_WHEREEVER, item, item->getItemCount(), nullptr, FLAG_NOLIMIT);
	}

	return true;
}

bool House::getAccessList(uint32_t listId, std::string& list) const
{
	if (listId == GUEST_LIST) {
		guestList.getList(list);
		return true;
	} else if (listId == SUBOWNER_LIST) {
		subOwnerList.getList(list);
		return true;
	}

	Door* door = getDoorByNumber(listId);
	if (!door) {
		return false;
	}

	return door->getAccessList(list);
}

bool House::isInvited(const Player* player)
{
	return getHouseAccessLevel(player) != HOUSE_NOT_INVITED;
}

void House::addDoor(Door* door)
{
	door->incrementReferenceCounter();
	doorList.push_back(door);
	door->setHouse(this);
	updateDoorDescription();
}

void House::removeDoor(Door* door)
{
	auto it = std::find(doorList.begin(), doorList.end(), door);
	if (it != doorList.end()) {
		door->decrementReferenceCounter();
		doorList.erase(it);
	}
}

void House::addBed(BedItem* bed)
{
	bedsList.push_back(bed);
	bed->setHouse(this);
}

Door* House::getDoorByNumber(uint32_t doorId) const
{
	for (Door* door : doorList) {
		if (door->getDoorId() == doorId) {
			return door;
		}
	}
	return nullptr;
}

Door* House::getDoorByPosition(const Position& pos)
{
	for (Door* door : doorList) {
		if (door->getPosition() == pos) {
			return door;
		}
	}
	return nullptr;
}

bool House::canEditAccessList(uint32_t listId, const Player* player)
{
	switch (getHouseAccessLevel(player)) {
		case HOUSE_OWNER:
			return true;

		case HOUSE_SUBOWNER:
			return listId == GUEST_LIST;

		default:
			return false;
	}
}

HouseTransferItem* House::getTransferItem()
{
	if (transferItem != nullptr) {
		return nullptr;
	}

	transfer_container.setParent(nullptr);
	transferItem = HouseTransferItem::createHouseTransferItem(this);
	transfer_container.addThing(transferItem);
	return transferItem;
}

void House::resetTransferItem()
{
	if (transferItem) {
		Item* tmpItem = transferItem;
		transferItem = nullptr;
		transfer_container.setParent(nullptr);

		transfer_container.removeThing(tmpItem, tmpItem->getItemCount());
		g_game.ReleaseItem(tmpItem);
	}
}

HouseTransferItem* HouseTransferItem::createHouseTransferItem(House* house)
{
	HouseTransferItem* transferItem = new HouseTransferItem(house);
	transferItem->incrementReferenceCounter();
	transferItem->setID(ITEM_DOCUMENT_RO);
	transferItem->setSubType(1);
	std::ostringstream ss;
	ss << "It is a house transfer document for '" << house->getName() << "'.";
	transferItem->setSpecialDescription(ss.str());
	return transferItem;
}

void HouseTransferItem::onTradeEvent(TradeEvents_t event, Player* owner)
{
	if (event == ON_TRADE_TRANSFER) {
		if (house) {
			house->executeTransfer(this, owner);
		}

		g_game.internalRemoveItem(this, 1);
	} else if (event == ON_TRADE_CANCEL) {
		if (house) {
			house->resetTransferItem();
		}
	}
}

bool House::executeTransfer(HouseTransferItem* item, Player* newOwner)
{
	if (transferItem != item) {
		return false;
	}

	setOwner(newOwner->getGUID());
	transferItem = nullptr;
	return true;
}

void AccessList::parseList(const std::string& list)
{
	playerList.clear();
	guildList.clear();
	allowEveryone = false;
	this->list = list;
	if (list.empty()) {
		return;
	}

	std::istringstream listStream(list);
	std::string line;

	int lineNo = 1;
	while (getline(listStream, line)) {
		if (++lineNo > 100) {
			break;
		}
		trimString(line);
		trim_left(line, '\t');
		trim_right(line, '\t');
		trimString(line);

		if (line.empty() || line.front() == '#' || line.length() > 100) {
			continue;
		}

		toLowerCaseString(line);

		std::string::size_type at_pos = line.find("@");
		if (at_pos != std::string::npos) {
			addGuild(line.substr(at_pos + 1));
		} else if (line.find("!") != std::string::npos || line.find("*") != std::string::npos || line.find("?") != std::string::npos) {
			continue; // regexp no longer supported
		} else {
			addPlayer(line);
		}
	}
}

void AccessList::addPlayer(const std::string& name)
{
	Player* player = g_game.getPlayerByName(name);
	if (player) {
		playerList.insert(player->getGUID());
	} else {
		uint32_t guid = IOLoginData::getGuidByName(name);
		if (guid != 0) {
			playerList.insert(guid);
		}
	}
}

void AccessList::addGuild(const std::string& name)
{
	uint32_t guildId = IOGuild::getGuildIdByName(name);
	if (guildId != 0) {
		guildList.insert(guildId);
	}
}

bool AccessList::isInList(const Player* player)
{
	if (allowEveryone) {
		return true;
	}

	auto playerIt = playerList.find(player->getGUID());
	if (playerIt != playerList.end()) {
		return true;
	}

	const Guild* guild = player->getGuild();
	return guild && guildList.find(guild->getId()) != guildList.end();
}

void AccessList::getList(std::string& list) const
{
	list = this->list;
}

Door::Door(uint16_t type) :	Item(type) {}

Attr_ReadValue Door::readAttr(AttrTypes_t attr, PropStream& propStream)
{
	if (attr == ATTR_HOUSEDOORID) {
		uint8_t doorId;
		if (!propStream.read<uint8_t>(doorId)) {
			return ATTR_READ_ERROR;
		}

		setDoorId(doorId);
		return ATTR_READ_CONTINUE;
	}
	return Item::readAttr(attr, propStream);
}

void Door::setHouse(House* house)
{
	if (this->house != nullptr) {
		return;
	}

	this->house = house;

	if (!accessList) {
		accessList.reset(new AccessList());
	}
}

bool Door::canUse(const Player* player)
{
	if (!house) {
		return true;
	}

	if (house->getHouseAccessLevel(player) >= HOUSE_SUBOWNER) {
		return true;
	}

	return accessList->isInList(player);
}

void Door::setAccessList(const std::string& textlist)
{
	if (!accessList) {
		accessList.reset(new AccessList());
	}

	accessList->parseList(textlist);
}

bool Door::getAccessList(std::string& list) const
{
	if (!house) {
		return false;
	}

	accessList->getList(list);
	return true;
}

void Door::onRemoved()
{
	Item::onRemoved();

	if (house) {
		house->removeDoor(this);
	}
}

House* Houses::getHouseByPlayerId(uint32_t playerId)
{
	for (const auto& it : houseMap) {
		if (it.second->getOwner() == playerId) {
			return it.second;
		}
	}
	return nullptr;
}

bool Houses::loadHousesXML(const std::string& filename)
{
	pugi::xml_document doc;
	pugi::xml_parse_result result = doc.load_file(filename.c_str());
	if (!result) {
		printXMLError("Error - Houses::loadHousesXML", filename, result);
		return false;
	}

	for (auto houseNode : doc.child("houses").children()) {
		pugi::xml_attribute houseIdAttribute = houseNode.attribute("houseid");
		if (!houseIdAttribute) {
			return false;
		}

		int32_t houseId = pugi::cast<int32_t>(houseIdAttribute.value());

		House* house = getHouse(houseId);
		if (!house) {
			std::cout << "Error: [Houses::loadHousesXML] Unknown house, id = " << houseId << std::endl;
			return false;
		}

		house->setName(houseNode.attribute("name").as_string());

		Position entryPos(
			pugi::cast<uint16_t>(houseNode.attribute("entryx").value()),
			pugi::cast<uint16_t>(houseNode.attribute("entryy").value()),
			pugi::cast<uint16_t>(houseNode.attribute("entryz").value())
		);
		if (entryPos.x == 0 && entryPos.y == 0 && entryPos.z == 0) {
			std::cout << "[Warning - Houses::loadHousesXML] House entry not set"
					    << " - Name: " << house->getName()
					    << " - House id: " << houseId << std::endl;
		}
		house->setEntryPos(entryPos);

		house->setRent(pugi::cast<uint32_t>(houseNode.attribute("rent").value()));
		house->setTownId(pugi::cast<uint32_t>(houseNode.attribute("townid").value()));

		house->setOwner(0, false);
	}
	return true;
}

void Houses::payHouses(RentPeriod_t rentPeriod) const
{
	if (rentPeriod == RENTPERIOD_NEVER) {
		return;
	}

	time_t currentTime = time(nullptr);
	for (const auto& it : houseMap) {
		House* house = it.second;
		if (house->getOwner() == 0) {
			continue;
		}

		const uint32_t rent = house->getRent();
		if (rent == 0 || house->getPaidUntil() > currentTime) {
			continue;
		}

		const uint32_t ownerId = house->getOwner();
		Town* town = g_game.map.towns.getTown(house->getTownId());
		if (!town) {
			continue;
		}

		Player player(nullptr);
		if (!IOLoginData::loadPlayerById(&player, ownerId)) {
			// Player doesn't exist, reset house owner
			house->setOwner(0);
			continue;
		}

		if (g_game.removeMoney(player.getDepotLocker(house->getTownId(), true), house->getRent(), FLAG_NOLIMIT)) {
			time_t paidUntil = currentTime;
			switch (rentPeriod) {
				case RENTPERIOD_DAILY:
					paidUntil += 24 * 60 * 60;
					break;
				case RENTPERIOD_WEEKLY:
					paidUntil += 24 * 60 * 60 * 7;
					break;
				case RENTPERIOD_MONTHLY:
					paidUntil += 24 * 60 * 60 * 30;
					break;
				case RENTPERIOD_YEARLY:
					paidUntil += 24 * 60 * 60 * 365;
					break;
				default:
					break;
			}

			house->setPaidUntil(paidUntil);
		} else {
			if (house->getPayRentWarnings() < 7) {
				int32_t daysLeft = 7 - house->getPayRentWarnings();

				Item* letter = Item::CreateItem(ITEM_LETTER_STAMPED);
				std::string period;

				switch (rentPeriod) {
					case RENTPERIOD_DAILY:
						period = "daily";
						break;

					case RENTPERIOD_WEEKLY:
						period = "weekly";
						break;

					case RENTPERIOD_MONTHLY:
						period = "monthly";
						break;

					case RENTPERIOD_YEARLY:
						period = "annual";
						break;

					default:
						break;
				}

				std::ostringstream ss;
				ss << "Warning! \nThe " << period << " rent of " << house->getRent() << " gold for your house \"" << house->getName() << "\" is payable. Have it within " << daysLeft << " days or you will lose this house.";
				letter->setText(ss.str());
				g_game.internalAddItem(player.getDepotLocker(house->getTownId(), true), letter, INDEX_WHEREEVER, FLAG_NOLIMIT);
				house->setPayRentWarnings(house->getPayRentWarnings() + 1);
			} else {
				house->setOwner(0, true, &player);
			}
		}

		IOLoginData::savePlayer(&player);
	}
}

void Houses::auctionsHouses(RentPeriod_t rentPeriod) const
{
	time_t currentTime;
	Database* db = Database::getInstance();
	DBResult_ptr result = db->storeQuery("SELECT `id`, `rent`, `town_id`, `bid_end`, `last_bid`, `highest_bidder` FROM `houses` WHERE `bid_end` > 0");
	int32_t banDay = g_config.getNumber(ConfigManager::BAN_ACCOUNT_FROM_BID_DAY);
	bool payRent = g_config.getBoolean(ConfigManager::FIRST_PAY_RENT_ON_FINAL_BID);

	if (result) {
		do {
			currentTime = time(nullptr);
			const uint32_t bid_end = result->getNumber<uint32_t>("bid_end");

			if (bid_end <= currentTime) {
				const uint32_t id = result->getNumber<uint32_t>("id");
				const uint32_t rent = result->getNumber<uint32_t>("rent");
				const uint32_t ownerId = result->getNumber<uint32_t>("highest_bidder");
				const uint32_t balance = result->getNumber<uint32_t>("last_bid");
				uint32_t rentTotal = 0;

				Player player(nullptr);
				if (!IOLoginData::loadPlayerById(&player, ownerId)) {
					std::ostringstream query;
					query << "UPDATE `houses` SET `owner` = 0, `bid` = 0, `bid_end` = 0, `last_bid` = 0, `highest_bidder` = 0  WHERE `id` = " << id;
					db->executeQuery(query.str());
				}
				else {
					House* house = g_game.map.houses.getHouse(id);
					Item* letter = Item::CreateItem(ITEM_LETTER_STAMPED);
					Town* town = g_game.map.towns.getTown(house->getTownId());

					if (!town) {
						continue;
					}

					bool bankMoney = false;

					if (payRent) {
						rentTotal = balance + rent;
					}
					else {
						rentTotal = balance;
					}

					if (player.getBankBalance() >= (rentTotal)) {
						bankMoney = true;
					}

					if (bankMoney || (g_game.removeMoney(player.getDepotLocker(house->getTownId(), true), rentTotal, FLAG_NOLIMIT) && !bankMoney)) {
						if (bankMoney) {
							player.setBankBalance(player.getBankBalance() - (rentTotal));
						}

						time_t paidUntil = currentTime;
						switch (rentPeriod) {
						case RENTPERIOD_DAILY:
							paidUntil += 24 * 60 * 60;
							break;
						case RENTPERIOD_WEEKLY:
							paidUntil += 24 * 60 * 60 * 7;
							break;
						case RENTPERIOD_MONTHLY:
							paidUntil += 24 * 60 * 60 * 30;
							break;
						case RENTPERIOD_YEARLY:
							paidUntil += 24 * 60 * 60 * 365;
							break;
						default:
							break;
						}

						std::ostringstream ss;
						ss << "Congratulations!\nYou won the auction.\n" << rentTotal << " gold has been deducted\nfrom your " << (bankMoney ? "bank balance" : "depot") << (payRent ? " as advance rent" : "") << ".\nRemember to save up for future rent payments.";
						letter->setText(ss.str());
						g_game.internalAddItem(player.getDepotLocker(house->getTownId(), true), letter, INDEX_WHEREEVER, FLAG_NOLIMIT);

						std::ostringstream query;
						query << "UPDATE `houses` SET `owner` = " << ownerId << ", `paid` = " << paidUntil << ", `bid` = 0, `bid_end` = 0, `last_bid` = 0, `highest_bidder` = 0  WHERE `id` = " << id;
						db->executeQuery(query.str());
						house->setPaidUntil(paidUntil);
						house->setOwner(ownerId, true, &player);
						std::cout << ">> House auction (" << id << ") set for " << player.getName() << ", payment from " << (bankMoney ? "bank: " : "depot: ") << rentTotal << " gold." << std::endl;
					}
					else {
						std::ostringstream query;
						if (banDay > 0) {
							std::ostringstream ss;
							ss << "Your account has been blocked from house auctions for " << banDay << " day(s)\n due to insufficient " << rentTotal << " gold missing in your " << (bankMoney ? "bank balance." : "depot.");
							letter->setText(ss.str());
							g_game.internalAddItem(player.getDepotLocker(house->getTownId(), true), letter, INDEX_WHEREEVER, FLAG_NOLIMIT);

							query << "UPDATE `accounts` SET `house_block` = " << std::to_string(currentTime += 24 * 60 * 60 * banDay) << " WHERE id = (SELECT account_id FROM players WHERE id = " << ownerId << ")";
							db->executeQuery(query.str());
							query.str("");
						}
						query << "UPDATE `houses` SET `owner` = 0, `bid` = 0, `bid_end` = 0, `last_bid` = 0, `highest_bidder` = 0  WHERE `id` = " << id;
						db->executeQuery(query.str());
						std::cout << ">> House auction (" << id << ") failed for " << player.getName() << " due to insufficient " << rentTotal << " gold in the " << (bankMoney ? "bank." : "depot.") << std::endl;
						query.str("");
					}
					IOLoginData::savePlayer(&player);
				}
			}
		} while (result->next());
	}
	std::cout << ">> Houses Auctions:" << (payRent ? " Advance rent enable!" : " Advance rent disable!") << (banDay > 0 ? " | Trolling auction ban for " + std::to_string(banDay) + " day(s)! " : " | Trolling auction ban disable!") << std::endl;
}