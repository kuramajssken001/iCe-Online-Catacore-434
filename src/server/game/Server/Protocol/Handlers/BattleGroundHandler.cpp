/*
 * Copyright (C) 2005-2011 MaNGOS <http://www.getmangos.com/>
 *
 * Copyright (C) 2008-2011 Trinity <http://www.trinitycore.org/>
 *
 * Copyright (C) 2010-2011 Project SkyFire <http://www.projectskyfire.org/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "gamePCH.h"
#include "Common.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "ArenaTeam.h"
#include "BattlefieldMgr.h"
#include "BattlegroundMgr.h"
#include "BattlegroundWS.h"
#include "BattlegroundTP.h"
#include "BattlegroundEY.h"
#include "Battleground.h"
#include "Chat.h"
#include "Language.h"
#include "Log.h"
#include "Player.h"
#include "Object.h"
#include "Opcodes.h"
#include "DisableMgr.h"

void WorldSession::HandleBattlemasterHelloOpcode(WorldPacket & recv_data)
{
    uint64 guid;
    recv_data >> guid;
    sLog->outDebug("WORLD: Recvd CMSG_BATTLEMASTER_HELLO Message from (GUID: %u TypeId:%u)", GUID_LOPART(guid),GuidHigh2TypeId(GUID_HIPART(guid)));

    Creature *unit = GetPlayer()->GetMap()->GetCreature(guid);
    if (!unit)
        return;

    if (!unit->isBattleMaster())                             // it's not battlemaster
        return;

    // Stop the npc if moving
    unit->StopMoving();

    SendBattlegGroundList(guid);
}

void WorldSession::SendBattlegGroundList(uint64 guid, BattlegroundTypeId bgTypeId)
{
    WorldPacket data;
    sBattlegroundMgr->BuildBattlegroundListPacket(&data, guid, _player, bgTypeId);
    SendPacket(&data);
}

void WorldSession::HandleBattlemasterJoinOpcode(WorldPacket & recv_data)
{
    uint32 bgTypeId_;
    uint32 instanceId;
    uint8 joinAsGroup;
    bool isPremade = false;
    Group *grp = NULL;
    ObjectGuid guid;

    recv_data >> instanceId;                 // Instance Id
    guid[2] = recv_data.ReadBit();
    guid[0] = recv_data.ReadBit();
    guid[3] = recv_data.ReadBit();
    guid[1] = recv_data.ReadBit();
    guid[5] = recv_data.ReadBit();
    joinAsGroup = recv_data.ReadBit();           // As Group
    guid[4] = recv_data.ReadBit();
    guid[6] = recv_data.ReadBit();
    guid[7] = recv_data.ReadBit();

    recv_data.ReadByteSeq(guid[2]);
    recv_data.ReadByteSeq(guid[6]);
    recv_data.ReadByteSeq(guid[4]);
    recv_data.ReadByteSeq(guid[3]);
    recv_data.ReadByteSeq(guid[7]);
    recv_data.ReadByteSeq(guid[0]);
    recv_data.ReadByteSeq(guid[5]);
    recv_data.ReadByteSeq(guid[1]);

    //extract from guid
    bgTypeId_ = GUID_LOPART(guid);

    if (!sBattlemasterListStore.LookupEntry(bgTypeId_))
    {
        sLog->outError("Battleground: invalid bgtype (%u) received. possible cheater? player guid %u",bgTypeId_,_player->GetGUIDLow());
        return;
    }

    if (sDisableMgr->IsDisabledFor(DISABLE_TYPE_BATTLEGROUND, bgTypeId_, NULL))
    {
        ChatHandler(this).PSendSysMessage(LANG_BG_DISABLED);
        return;
    }

    BattlegroundTypeId bgTypeId = BattlegroundTypeId(bgTypeId_);

    /* force random bg */
    if (_player->getLevel() >= 80) {
        switch (bgTypeId) {
            case BATTLEGROUND_WS:
            case BATTLEGROUND_AB:
            case BATTLEGROUND_EY:
            case BATTLEGROUND_TP:
            case BATTLEGROUND_BG:
                bgTypeId = BATTLEGROUND_RB;
                break;
            default:
                break;
        }
    }

    // can do this, since it's battleground, not arena
    BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, 0);
    BattlegroundQueueTypeId bgQueueTypeIdRandom = BattlegroundMgr::BGQueueTypeId(BATTLEGROUND_RB, 0);

    // ignore if player is already in BG
    if (_player->InBattleground())
        return;

    // get bg instance or bg template if instance not found
    Battleground *bg = NULL;
    //if (instanceId)
    //    bg = sBattlegroundMgr.GetBattlegroundThroughClientInstance(instanceId, bgTypeId);

    if (!bg)
        bg = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);

    if (!bg)
    {
        sLog->outError("Battleground template for BG Type ID %u not found!",bgTypeId_);
        return;
    }

    // expected bracket entry
    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bg->GetMapId(),_player->getLevel());
    if (!bracketEntry)
    {
        sLog->outError("Unexpected level branch branch for player level %u",_player->getLevel());
        return;
    }

    GroupJoinBattlegroundResult err;

    // check queue conditions
    if (joinAsGroup == 0)
    {
        if (_player->isUsingLfg())
        {
            // player is using dungeon finder or raid finder
            WorldPacket data;
            sBattlegroundMgr->BuildStatusFailedPacket(&data, bg, _player, 0, ERR_LFG_CANT_USE_BATTLEGROUND);
            GetPlayer()->GetSession()->SendPacket(&data);
            return;
        }

        // check Deserter debuff
        if (!_player->CanJoinToBattleground())
        {
            WorldPacket data;
            sBattlegroundMgr->BuildStatusFailedPacket(&data, bg, _player, 0, ERR_GROUP_JOIN_BATTLEGROUND_DESERTERS);
            _player->GetSession()->SendPacket(&data);
            return;
        }

        if (_player->GetBattlegroundQueueIndex(bgQueueTypeIdRandom) < PLAYER_MAX_BATTLEGROUND_QUEUES)
        {
            //player is already in random queue
            WorldPacket data;
            sBattlegroundMgr->BuildStatusFailedPacket(&data, bg, _player, 0, ERR_IN_RANDOM_BG);
            _player->GetSession()->SendPacket(&data);
            return;
        }

        if ((_player->InBattlegroundQueue() || _player->getLevel() < 80) && bgTypeId == BATTLEGROUND_RB)
        {
            //player is already in queue, can't start random queue
            WorldPacket data;
            sBattlegroundMgr->BuildStatusFailedPacket(&data, bg, _player, 0, ERR_IN_NON_RANDOM_BG);
            _player->GetSession()->SendPacket(&data);
            return;
        }

        // check if already in queue
        if (_player->GetBattlegroundQueueIndex(bgQueueTypeId) < PLAYER_MAX_BATTLEGROUND_QUEUES)
        {
            //player is already in this queue
            return;
        }

        // check if has free queue slots
        if (!_player->HasFreeBattlegroundQueueId())
        {
            WorldPacket data;
            sBattlegroundMgr->BuildStatusFailedPacket(&data, bg, _player, 0, ERR_BATTLEGROUND_TOO_MANY_QUEUES);
            _player->GetSession()->SendPacket(&data);
            return;
        }

        BattlegroundQueue& bgQueue = sBattlegroundMgr->m_BattlegroundQueues[bgQueueTypeId];

        GroupQueueInfo *ginfo = bgQueue.AddGroup(_player, NULL, bgTypeId, bracketEntry, 0, false, isPremade, 0, 0);
        uint32 avgTime = bgQueue.GetAverageQueueWaitTime(ginfo, bracketEntry->GetBracketId());
        // already checked if queueSlot is valid, now just get it
        uint32 queueSlot = _player->AddBattlegroundQueueId(bgQueueTypeId);

        WorldPacket data;
                                                            // send status packet (in queue)
        sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, _player, queueSlot, STATUS_WAIT_QUEUE, avgTime, ginfo->JoinTime, ginfo->ArenaType);
        SendPacket(&data);
        sLog->outDebug("Battleground: player joined queue for bg queue type %u bg type %u: GUID %u, NAME %s",bgQueueTypeId,bgTypeId,_player->GetGUIDLow(), _player->GetName());
    }
    else
    {
        grp = _player->GetGroup();
        // no group found, error
        if (!grp)
            return;
        if (grp->GetLeaderGUID() != _player->GetGUID())
            return;
        err = grp->CanJoinBattlegroundQueue(bg, bgQueueTypeId, 0, bg->GetMaxPlayersPerTeam(), false, 0);
        isPremade = (grp->GetMembersCount() >= bg->GetMinPlayersPerTeam());

        BattlegroundQueue &bgQueue = sBattlegroundMgr->m_BattlegroundQueues[bgQueueTypeId];
        GroupQueueInfo * ginfo = NULL;
        uint32 avgTime = 0;

        if (err == ERR_BATTLEGROUND_NONE)
        {
            sLog->outDebug("Battleground: the following players are joining as group:");
            ginfo = bgQueue.AddGroup(_player, grp, bgTypeId, bracketEntry, 0, false, isPremade, 0, 0);
            avgTime = bgQueue.GetAverageQueueWaitTime(ginfo, bracketEntry->GetBracketId());
        }

        for (GroupReference *itr = grp->GetFirstMember(); itr != NULL; itr = itr->next())
        {
            Player *member = itr->getSource();
            if (!member) continue;   // this should never happen

            WorldPacket data;

            if (err != ERR_BATTLEGROUND_NONE)
            {
                sBattlegroundMgr->BuildStatusFailedPacket(&data, bg, _player, 0, err);
                member->GetSession()->SendPacket(&data);
                continue;
            }

            // add to queue
            uint32 queueSlot = member->AddBattlegroundQueueId(bgQueueTypeId);

            // send status packet (in queue)
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, member, queueSlot, STATUS_WAIT_QUEUE, avgTime, ginfo->JoinTime, ginfo->ArenaType);
            member->GetSession()->SendPacket(&data);
            sLog->outDebug("Battleground: player joined queue for bg queue type %u bg type %u: GUID %u, NAME %s",bgQueueTypeId,bgTypeId,member->GetGUIDLow(), member->GetName());
        }
        sLog->outDebug("Battleground: group end");

    }
    sBattlegroundMgr->ScheduleQueueUpdate(0, 0, bgQueueTypeId, bgTypeId, bracketEntry->GetBracketId());
}

void WorldSession::HandleBattlegroundPlayerPositionsOpcode(WorldPacket &recv_data)
{
    sLog->outDebug("WORLD: Recvd CMSG_BATTLEGROUND_PLAYER_POSITIONS Message");

    Battleground *bg = _player->GetBattleground();
    if (!bg)                                                 // can't be received if player not in battleground
        return;

    uint32 acount = 0;
    uint32 hcount = 0;
    Player *aplr = NULL;
    Player *hplr = NULL;

    if (uint64 guid = bg->GetFlagPickerGUID(TEAM_ALLIANCE))
    {
        aplr = ObjectAccessor::FindPlayer(guid);
        if (aplr)
            ++acount;
    }

    if (uint64 guid = bg->GetFlagPickerGUID(TEAM_HORDE))
    {
        hplr = ObjectAccessor::FindPlayer(guid);
        if (hplr)
            ++hcount;
    }

    ObjectGuid aguid = aplr ? aplr->GetGUID() : 0;
    ObjectGuid hguid = hplr ? hplr->GetGUID() : 0;

    WorldPacket data(SMSG_BATTLEFIELD_PLAYER_POSITIONS);

    data.WriteBits(acount, 22);
    for (uint8 i = 0; i < acount; i++)
    {
        data.WriteBit(aguid[3]);
        data.WriteBit(aguid[5]);
        data.WriteBit(aguid[1]);
        data.WriteBit(aguid[6]);
        data.WriteBit(aguid[7]);
        data.WriteBit(aguid[0]);
        data.WriteBit(aguid[2]);
        data.WriteBit(aguid[4]);
    }

    data.WriteBits(hcount, 22);
    for (uint8 i = 0; i < hcount; i++)
    {
        data.WriteBit(hguid[6]);
        data.WriteBit(hguid[5]);
        data.WriteBit(hguid[4]);
        data.WriteBit(hguid[7]);
        data.WriteBit(hguid[2]);
        data.WriteBit(hguid[1]);
        data.WriteBit(hguid[0]);
        data.WriteBit(hguid[3]);
    }

    data.FlushBits();

    for (uint8 i = 0; i < hcount; i++)
    {
        data.WriteByteSeq(hguid[2]);
        data.WriteByteSeq(hguid[1]);
        data << float(hplr->GetPositionY());
        data.WriteByteSeq(hguid[5]);
        data.WriteByteSeq(hguid[4]);
        data.WriteByteSeq(hguid[7]);
        data.WriteByteSeq(hguid[0]);
        data.WriteByteSeq(hguid[6]);
        data.WriteByteSeq(hguid[3]);
        data << float(hplr->GetPositionX());
    }

    for (uint8 i = 0; i < acount; i++)
    {
        data.WriteByteSeq(aguid[6]);
        data << float(aplr->GetPositionX());
        data.WriteByteSeq(aguid[5]);
        data.WriteByteSeq(aguid[3]);
        data << float(aplr->GetPositionY());
        data.WriteByteSeq(aguid[1]);
        data.WriteByteSeq(aguid[7]);
        data.WriteByteSeq(aguid[0]);
        data.WriteByteSeq(aguid[2]);
        data.WriteByteSeq(aguid[4]);
    }

    SendPacket(&data);
}

void WorldSession::HandlePVPLogDataOpcode(WorldPacket & /*recv_data*/)
{
    sLog->outDebug("WORLD: Recvd MSG_PVP_LOG_DATA Message");

    Battleground *bg = _player->GetBattleground();
    if (!bg)
        return;

    WorldPacket data;
    sBattlegroundMgr->BuildPvpLogDataPacket(&data, bg);
    SendPacket(&data);

    sLog->outDebug("WORLD: Sent MSG_PVP_LOG_DATA Message");
}

void WorldSession::HandleBattlefieldListOpcode(WorldPacket &recv_data)
{
    sLog->outDebug("WORLD: Recvd CMSG_BATTLEFIELD_LIST Message");

    uint8 bgTypeId;
    recv_data >> bgTypeId;                                  // id from DBC

    WorldPacket data;
    sBattlegroundMgr->BuildBattlegroundListPacket(&data, 0, _player, BattlegroundTypeId(bgTypeId));
    SendPacket(&data);
}

void WorldSession::HandleBattleFieldPortOpcode(WorldPacket &recv_data)
{
    sLog->outDebug("WORLD: Recvd CMSG_BATTLEFIELD_PORT Message");

    uint32 time;
    uint32 queueSlot;
    uint32 unk;
    uint8 action;                       // enter battle 1, leave queue 0
    ObjectGuid guid;

    recv_data >> time;
    recv_data >> queueSlot;
    recv_data >> unk;

    guid[0] = recv_data.ReadBit();
    guid[1] = recv_data.ReadBit();
    guid[5] = recv_data.ReadBit();
    guid[6] = recv_data.ReadBit();
    guid[7] = recv_data.ReadBit();
    guid[4] = recv_data.ReadBit();
    guid[3] = recv_data.ReadBit();
    guid[2] = recv_data.ReadBit();

    action = recv_data.ReadBit();

    recv_data.ReadByteSeq(guid[1]);
    recv_data.ReadByteSeq(guid[3]);
    recv_data.ReadByteSeq(guid[5]);
    recv_data.ReadByteSeq(guid[7]);
    recv_data.ReadByteSeq(guid[0]);
    recv_data.ReadByteSeq(guid[2]);
    recv_data.ReadByteSeq(guid[6]);
    recv_data.ReadByteSeq(guid[4]);

    if (!_player->InBattlegroundQueue())
    {
        sLog->outError("BattlegroundHandler: Invalid CMSG_BATTLEFIELD_PORT received from player (Name: %s, GUID: %u), he is not in bg_queue.", _player->GetName(), _player->GetGUIDLow());
        return;
    }

    BattlegroundQueueTypeId bgQueueTypeId = _player->GetBattlegroundQueueTypeId(queueSlot);
    if (bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
    {
        sLog->outError("BattlegroundHandler: invalid queueSlot (%u) received.", queueSlot);
        return;
    }

    BattlegroundQueue &bgQueue = sBattlegroundMgr->m_BattlegroundQueues[bgQueueTypeId];
    BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(bgQueueTypeId);

    //we must use temporary variable, because GroupQueueInfo pointer can be deleted in BattlegroundQueue::RemovePlayer() function
    GroupQueueInfo ginfo;
    if (!bgQueue.GetPlayerGroupInfoData(_player->GetGUID(), &ginfo))
    {
        sLog->outError("BattlegroundHandler: itrplayerstatus not found.");
        return;
    }
    // if action == 1, then instanceId is required
    if (!ginfo.IsInvitedToBGInstanceGUID && action == 1)
    {
        sLog->outError("BattlegroundHandler: instance not found.");
        return;
    }

    Battleground *bg = sBattlegroundMgr->GetBattleground(ginfo.IsInvitedToBGInstanceGUID, bgTypeId == BATTLEGROUND_AA ? BATTLEGROUND_TYPE_NONE : bgTypeId);

    // bg template might and must be used in case of leaving queue, when instance is not created yet
    if (!bg && action == 0)
        bg = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    if (!bg)
    {
        sLog->outError("BattlegroundHandler: bg_template not found for type id %u.", bgTypeId);
        return;
    }
    
    if (bgTypeId == BATTLEGROUND_AA)
        bgTypeId = bg->GetTypeID();

    // expected bracket entry
    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bg->GetMapId(), _player->getLevel());
    if (!bracketEntry)
    {
        sLog->outError("BattlegroundHandler: Unexpected level bracket request for level %u",_player->getLevel());
        return;
    }

    //some checks if player isn't cheating - it is not exactly cheating, but we cannot allow it
    if (action == 1 && ginfo.ArenaType == 0)
    {
        //if player is trying to enter battleground (not arena!) and he has deserter debuff, we must just remove him from queue
        if (!_player->CanJoinToBattleground())
        {
            //send bg command result to show nice message
            WorldPacket data2;
            sBattlegroundMgr->BuildStatusFailedPacket(&data2, bg, _player, 0, ERR_GROUP_JOIN_BATTLEGROUND_DESERTERS);
            _player->GetSession()->SendPacket(&data2);
            action = 0;
            sLog->outDebug("Battleground: player %s (%u) has a deserter debuff, do not port him to battleground!", _player->GetName(), _player->GetGUIDLow());
        }
        //if player don't match battleground max level, then do not allow him to enter! (this might happen when player leveled up during his waiting in queue
        if (_player->getLevel() > bg->GetMaxLevel())
        {
            sLog->outError("Battleground: Player %s (%u) has level (%u) higher than maxlevel (%u) of battleground (%u)! Do not port him to battleground!",
                _player->GetName(), _player->GetGUIDLow(), _player->getLevel(), bg->GetMaxLevel(), bg->GetTypeID());
            action = 0;
        }
    }

    WorldPacket data;
    switch (action)
    {
        case 1:                                         // port to battleground
            if (!_player->IsInvitedForBattlegroundQueueType(bgQueueTypeId))
                return;                                 // cheating?

            if (!_player->InBattleground())
                _player->SetBattlegroundEntryPoint();

            // resurrect the player
            if (!_player->isAlive())
            {
                _player->ResurrectPlayer(1.0f);
                _player->SpawnCorpseBones();
            }
            // stop taxi flight at port
            if (_player->isInFlight())
            {
                _player->GetMotionMaster()->MovementExpired();
                _player->CleanupAfterTaxiFlight();
            }

            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, _player, queueSlot, STATUS_IN_PROGRESS, 0, bg->GetStartTime(), bg->GetArenaType());
            _player->GetSession()->SendPacket(&data);
            // remove battleground queue status from BGmgr
            bgQueue.RemovePlayer(_player->GetGUID(), false);
            // this is still needed here if battleground "jumping" shouldn't add deserter debuff
            // also this is required to prevent stuck at old battleground after SetBattlegroundId set to new
            if (Battleground *currentBg = _player->GetBattleground())
                currentBg->RemovePlayerAtLeave(_player->GetGUID(), false, true);

            // set the destination instance id
            _player->SetBattlegroundId(bg->GetInstanceID(), bgTypeId);
            // set the destination team
            _player->SetBGTeam(ginfo.Team);
            // bg->HandleBeforeTeleportToBattleground(_player);
            sBattlegroundMgr->SendToBattleground(_player, ginfo.IsInvitedToBGInstanceGUID, bgTypeId);
            // add only in HandleMoveWorldPortAck()
            // bg->AddPlayer(_player, team);
            sLog->outDebug("Battleground: player %s (%u) joined battle for bg %u, bgtype %u, queue type %u.", _player->GetName(), _player->GetGUIDLow(), bg->GetInstanceID(), bg->GetTypeID(), bgQueueTypeId);
            break;
        case 0:                                         // leave queue
            if (bg->isArena() && bg->GetStatus() > STATUS_WAIT_QUEUE)
                return;

            // if player leaves rated arena match before match start, it is counted as he played but he lost
            if (ginfo.IsRated && ginfo.IsInvitedToBGInstanceGUID)
            {
                ArenaTeam *at = sObjectMgr->GetArenaTeamById(ginfo.Team);
                if (at)
                {
                    sLog->outDebug("UPDATING memberLost's personal arena rating for %u by opponents rating: %u, because he has left queue!", GUID_LOPART(_player->GetGUID()), ginfo.OpponentsTeamRating);
                    at->MemberLost(_player, ginfo.OpponentsMatchmakerRating);
                    at->SaveToDB();
                }
            }
            _player->RemoveBattlegroundQueueId(bgQueueTypeId);  // must be called this way, because if you move this call to queue->removeplayer, it causes bugs
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, _player, queueSlot, STATUS_NONE, 0, 0, 0);
            bgQueue.RemovePlayer(_player->GetGUID(), true);
            // player left queue, we should update it - do not update Arena Queue
            if (!ginfo.ArenaType)
                sBattlegroundMgr->ScheduleQueueUpdate(ginfo.ArenaMatchmakerRating, ginfo.ArenaType, bgQueueTypeId, bgTypeId, bracketEntry->GetBracketId());
            SendPacket(&data);
            sLog->outDebug("Battleground: player %s (%u) left queue for bgtype %u, queue type %u.", _player->GetName(), _player->GetGUIDLow(), bg->GetTypeID(), bgQueueTypeId);
            break;
        default:
            sLog->outError("Battleground port: unknown action %u", action);
            break;
    }
}

void WorldSession::HandleLeaveBattlefieldOpcode(WorldPacket& recv_data)
{
    sLog->outDebug("WORLD: Recvd CMSG_LEAVE_BATTLEFIELD Message");

    // not allow leave battleground in combat
    if (_player->isInCombat())
        if (Battleground* bg = _player->GetBattleground())
            if (bg->GetStatus() != STATUS_WAIT_LEAVE)
                return;

    _player->LeaveBattleground();
}

void WorldSession::HandleBattlefieldStatusOpcode(WorldPacket & /*recv_data*/)
{
    // empty opcode
    sLog->outDebug("WORLD: Battleground status");

    WorldPacket data;
    // we must update all queues here
    Battleground *bg = NULL;
    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
    {
        BattlegroundQueueTypeId bgQueueTypeId = _player->GetBattlegroundQueueTypeId(i);
        if (!bgQueueTypeId)
            continue;
        BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(bgQueueTypeId);
        uint8 arenaType = BattlegroundMgr::BGArenaType(bgQueueTypeId);
        if (bgTypeId == _player->GetBattlegroundTypeId())
        {
            bg = _player->GetBattleground();
            //i cannot check any variable from player class because player class doesn't know if player is in 2v2 / 3v3 or 5v5 arena
            //so i must use bg pointer to get that information
            if (bg && bg->GetArenaType() == arenaType)
            {
                // this line is checked, i only don't know if GetStartTime is changing itself after bg end!
                // send status in Battleground
                sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, _player, i, STATUS_IN_PROGRESS, bg->GetEndTime(), bg->GetStartTime(), arenaType);
                SendPacket(&data);
                continue;
            }
        }
        //we are sending update to player about queue - he can be invited there!
        //get GroupQueueInfo for queue status
        BattlegroundQueue& bgQueue = sBattlegroundMgr->m_BattlegroundQueues[bgQueueTypeId];
        GroupQueueInfo ginfo;
        if (!bgQueue.GetPlayerGroupInfoData(_player->GetGUID(), &ginfo))
            continue;
        if (ginfo.IsInvitedToBGInstanceGUID)
        {
            bg = sBattlegroundMgr->GetBattleground(ginfo.IsInvitedToBGInstanceGUID, bgTypeId);
            if (!bg)
                continue;
            uint32 remainingTime = getMSTimeDiff(getMSTime(), ginfo.RemoveInviteTime);
            // send status invited to Battleground
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, _player, i, STATUS_WAIT_JOIN, remainingTime, 0, arenaType);
            SendPacket(&data);
        }
        else
        {
            bg = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
            if (!bg)
                continue;

            // expected bracket entry
            PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bg->GetMapId(),_player->getLevel());
            if (!bracketEntry)
                continue;

            uint32 avgTime = bgQueue.GetAverageQueueWaitTime(&ginfo, bracketEntry->GetBracketId());
            // send status in Battleground Queue
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, _player, i, STATUS_WAIT_QUEUE, avgTime, getMSTimeDiff(ginfo.JoinTime, getMSTime()), arenaType);
            SendPacket(&data);
        }
    }
}

void WorldSession::HandleAreaSpiritHealerQueryOpcode(WorldPacket & recv_data)
{
    sLog->outDebug("WORLD: CMSG_AREA_SPIRIT_HEALER_QUERY");

    Battleground *bg = _player->GetBattleground();

    uint64 guid;
    recv_data >> guid;

    Creature *unit = GetPlayer()->GetMap()->GetCreature(guid);
    if (!unit)
        return;

    if (!unit->isSpiritService())                            // it's not spirit service
        return;

    if (bg)
        sBattlegroundMgr->SendAreaSpiritHealerQueryOpcode(_player, bg, guid);

    if (Battlefield* bf = sBattlefieldMgr.GetBattlefieldToZoneId(_player->GetZoneId()))
        bf->SendAreaSpiritHealerQueryOpcode(_player,guid);
}


void WorldSession::HandleAreaSpiritHealerQueueOpcode(WorldPacket & recv_data)
{
    sLog->outDebug("WORLD: CMSG_AREA_SPIRIT_HEALER_QUEUE");

    Battleground *bg = _player->GetBattleground();

    uint64 guid;
    recv_data >> guid;

    Creature *unit = GetPlayer()->GetMap()->GetCreature(guid);
    if (!unit)
        return;

    if (!unit->isSpiritService())                            // it's not spirit service
        return;

    if (bg)
        bg->AddPlayerToResurrectQueue(guid, _player->GetGUID());

    if (Battlefield* bf = sBattlefieldMgr.GetBattlefieldToZoneId(_player->GetZoneId()))
        bf->AddPlayerToResurrectQueue(guid, _player->GetGUID());
}

void WorldSession::HandleRequestPvPOptions(WorldPacket &recv_data)
{
    // TODO: implement
}

void WorldSession::HandleBattlemasterJoinArena(WorldPacket & recv_data)
{
    sLog->outDebug("WORLD: CMSG_BATTLEMASTER_JOIN_ARENA");
    //recv_data.hexlike();

    uint8 arenaslot;                                        // 2v2, 3v3 or 5v5
    Group * grp = NULL;

    bool isRated = true, asGroup = true;

    recv_data >> arenaslot;

    // ignore if we already in BG or BG queue
    if (_player->InBattleground())
        return;

    uint8 arenatype = 0;
    uint32 arenaRating = 0;
    uint32 matchmakerRating = 0;

    switch(arenaslot)
    {
        case 0:
            arenatype = ARENA_TYPE_2v2;
            break;
        case 1:
            arenatype = ARENA_TYPE_3v3;
            break;
        case 2:
            arenatype = ARENA_TYPE_5v5;
            break;
        default:
            sLog->outError("Unknown arena slot %u at HandleBattlemasterJoinArena()", arenaslot);
            return;
    }

    //check existance
    Battleground* bg = sBattlegroundMgr->GetBattlegroundTemplate(BATTLEGROUND_AA);
    if (!bg)
    {
        sLog->outError("Battleground: template bg (all arenas) not found");
        return;
    }

    if (sDisableMgr->IsDisabledFor(DISABLE_TYPE_BATTLEGROUND, BATTLEGROUND_AA, NULL))
    {
        ChatHandler(this).PSendSysMessage(LANG_ARENA_DISABLED);
        return;
    }

    BattlegroundTypeId bgTypeId = bg->GetTypeID();
    BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, arenatype);
    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bg->GetMapId(),_player->getLevel());
    if (!bracketEntry)
        return;

    GroupJoinBattlegroundResult err = ERR_BATTLEGROUND_NONE;

    if (!asGroup)
    {
        // check if already in queue
        if (_player->GetBattlegroundQueueIndex(bgQueueTypeId) < PLAYER_MAX_BATTLEGROUND_QUEUES)
            //player is already in this queue
            return;
        // check if has free queue slots
        if (!_player->HasFreeBattlegroundQueueId())
            return;
    }
    else
    {
        grp = _player->GetGroup();
        // no group found, error
        if (!grp)
            return;
        if (grp->GetLeaderGUID() != _player->GetGUID())
            return;
        err = grp->CanJoinBattlegroundQueue(bg, bgQueueTypeId, arenatype, arenatype, (bool)isRated, arenaslot);
    }

    uint32 ateamId = 0;

    if (isRated)
    {
        ateamId = _player->GetArenaTeamId(arenaslot);
        // check real arenateam existence only here (if it was moved to group->CanJoin .. () then we would ahve to get it twice)
        ArenaTeam * at = sObjectMgr->GetArenaTeamById(ateamId);
        if (!at)
        {
            _player->GetSession()->SendNotInArenaTeamPacket(arenatype);
            return;
        }
        // get the team rating for queueing
        arenaRating = at->GetRating();
        matchmakerRating = at->GetAverageMMR(grp);
        // the arenateam id must match for everyone in the group

        if (arenaRating <= 0)
            arenaRating = 1;
    }

    BattlegroundQueue &bgQueue = sBattlegroundMgr->m_BattlegroundQueues[bgQueueTypeId];
    if (asGroup)
    {
        uint32 avgTime = 0;

        if (err == ERR_BATTLEGROUND_NONE)
        {
            sLog->outDebug("Battleground: arena join as group start");
            if (isRated)
            {
                sLog->outDebug("Battleground: arena team id %u, leader %s queued with matchmaker rating %u for type %u",_player->GetArenaTeamId(arenaslot),_player->GetName(),matchmakerRating,arenatype);
                bg->SetRated(true);
            }
            else
                bg->SetRated(false);

            GroupQueueInfo * ginfo = bgQueue.AddGroup(_player, grp, bgTypeId, bracketEntry, arenatype, isRated, false, arenaRating, matchmakerRating, ateamId);
            avgTime = bgQueue.GetAverageQueueWaitTime(ginfo, bracketEntry->GetBracketId());
        }

        for (GroupReference *itr = grp->GetFirstMember(); itr != NULL; itr = itr->next())
        {
            Player *member = itr->getSource();
            if (!member)
                continue;

            WorldPacket data;

            if (err != ERR_BATTLEGROUND_NONE)
            {
                sBattlegroundMgr->BuildStatusFailedPacket(&data, bg, _player, 0, err);
                member->GetSession()->SendPacket(&data);
                continue;
            }

            // add to queue
            uint32 queueSlot = member->AddBattlegroundQueueId(bgQueueTypeId);

            // send status packet (in queue)
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, member, queueSlot, STATUS_WAIT_QUEUE, avgTime, 0, arenatype);
            member->GetSession()->SendPacket(&data);
            sLog->outDebug("Battleground: player joined queue for arena as group bg queue type %u bg type %u: GUID %u, NAME %s",bgQueueTypeId,bgTypeId,member->GetGUIDLow(), member->GetName());
        }
    }
    else
    {
        GroupQueueInfo * ginfo = bgQueue.AddGroup(_player, NULL, bgTypeId, bracketEntry, arenatype, isRated, false, arenaRating, matchmakerRating, ateamId);
        uint32 avgTime = bgQueue.GetAverageQueueWaitTime(ginfo, bracketEntry->GetBracketId());
        uint32 queueSlot = _player->AddBattlegroundQueueId(bgQueueTypeId);

        WorldPacket data;
        // send status packet (in queue)
        sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, _player, queueSlot, STATUS_WAIT_QUEUE, avgTime, 0, arenatype);
        SendPacket(&data);
        sLog->outDebug("Battleground: player joined queue for arena, skirmish, bg queue type %u bg type %u: GUID %u, NAME %s",bgQueueTypeId,bgTypeId,_player->GetGUIDLow(), _player->GetName());
    }
    sBattlegroundMgr->ScheduleQueueUpdate(matchmakerRating, arenatype, bgQueueTypeId, bgTypeId, bracketEntry->GetBracketId());
}

void WorldSession::HandleReportPvPAFK(WorldPacket & recv_data)
{
    uint64 playerGuid;
    recv_data >> playerGuid;
    Player *reportedPlayer = sObjectMgr->GetPlayer(playerGuid);

    if (!reportedPlayer)
    {
        sLog->outDebug("WorldSession::HandleReportPvPAFK: player not found");
        return;
    }

    sLog->outDebug("WorldSession::HandleReportPvPAFK: %s reported %s", _player->GetName(), reportedPlayer->GetName());

    reportedPlayer->ReportedAfkBy(_player);
}

void WorldSession::HandleRequestRatedBgInfo(WorldPacket& recv_data)
{
    /*
    uint8 unk_byte;
    recv_data >> unk_byte; // always 3

    uint32 rating = _player->GetRatedBattlegroundRating();
    uint32 current_CP = _player->GetCurrencyWeekCount(CURRENCY_TYPE_CONQUEST_POINT, CURRENCY_SOURCE_BG);
    uint32 CP_cap = _player->GetCurrencyWeekCap(CURRENCY_TYPE_CONQUEST_POINT, CURRENCY_SOURCE_BG);
    uint32 CP_for_win = (uint32)std::ceil(_player->GetCurrencyWeekCap(CURRENCY_TYPE_CONQUEST_POINT, CURRENCY_SOURCE_BG) / 6.0f);

    WorldPacket data(SMSG_RATED_BATTLEFIELD_INFO, 5 * 4 + 1);
    data << uint32(rating);             // rating
    data << uint8(3);                   // unk byte, always 3
    data << uint32(CP_cap);             // conquest point weekly cap
    data << uint32(CP_for_win);         // conquest points for win
    data << uint32(0);                  // unk
    data << uint32(current_CP);         // current conquest points
    SendPacket(&data);

    sLog->outDebug("WorldSession::HandleRequestRatedBgInfo: Player %s (GUID: %u) requested rated BG info", _player->GetName(), _player->GetGUIDLow());
    */
}

void WorldSession::HandleRequestRatedBgStats(WorldPacket& recv_data)
{
    /*
    // client calculation
    // 11th uint32 + 15th uint32 + 17th uint32 == total games
    // 1st uint32 + 12th uint32 + 18th uint32 == games won
    // 4th uint32 + 5th uint32 + 9th uint32 == unk1
    // 6th uint32 + 13th uint32 + 16th uint32 == unk2
    // rest is unknown

    uint32 matches_won = _player->GetRatedBattlegroundStat(RATED_BG_STAT_MATCHES_WON);
    uint32 matches_lost = _player->GetRatedBattlegroundStat(RATED_BG_STAT_MATCHES_LOST);

    WorldPacket data(SMSG_PVP_RATED_STATS_UPDATE, 18 * 4);
    data << uint32(matches_won); // games won
    data << uint32(0);
    data << uint32(0);
    data << uint32(0); // unk1
    data << uint32(0);
    data << uint32(0); // unk2
    data << uint32(0);
    data << uint32(0);
    data << uint32(0);
    data << uint32(0);
    data << uint32(matches_won + matches_lost); // total games
    data << uint32(0);
    data << uint32(0);
    data << uint32(0);
    data << uint32(0);
    data << uint32(0);
    data << uint32(0);
    data << uint32(0);
    SendPacket(&data);

    sLog->outDebug("WorldSession::HandleRequestRatedBgInfo: Player %s (GUID: %u) requested rated BG stats", _player->GetName(), _player->GetGUIDLow());
    */
}

void WorldSession::HandleBattlemasterJoinRated(WorldPacket& recv_data)
{
    if (!(sWorld->getBoolConfig(CONFIG_RATED_BATTLEGROUND_ENABLED)))
    {
        SendNotification("Rated battlegrounds are currently disabled.");
        return;
    }

    BattlegroundTypeId bgTypeId = sBattlegroundMgr->GetRatedBattlegroundType();
    uint32 arenaType = sBattlegroundMgr->GetRatedBattlegroundSize();

    Battleground *bg = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    if (!bg)
    {
        sLog->outError("Battleground: template for rated BG not found!");
        return;
    }

    bg->SetRated(true);
    bg->SetArenaType(arenaType);

    BattlegroundQueueTypeId queueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, arenaType);
    BattlegroundQueue &queue = sBattlegroundMgr->m_BattlegroundQueues[queueTypeId];
    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bg->GetMapId(), _player->getLevel());

    Group *group = _player->GetGroup();

    GroupJoinBattlegroundResult err = group->CanJoinBattlegroundQueue(bg, queueTypeId, arenaType, arenaType, true, 0);
    uint32 avgTime = 0;
    uint32 avgRating = 0;

    if (err != ERR_BATTLEGROUND_NONE)
    {
        avgRating = group->GetAverageBattlegroundRating();
        GroupQueueInfo *groupQueueInfo = queue.AddGroup(_player, group, bgTypeId, bracketEntry, arenaType, true, true, avgRating, avgRating);
        avgTime = queue.GetAverageQueueWaitTime(groupQueueInfo, bracketEntry->GetBracketId());
    }

    for (GroupReference *itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* member = itr->getSource();
        if (!member)
            continue;

        WorldPacket data;

        if (err != ERR_BATTLEGROUND_NONE)
        {
            sBattlegroundMgr->BuildStatusFailedPacket(&data, bg, _player, 0, err);
            member->GetSession()->SendPacket(&data);
            continue;
        }

        // add to queue
        uint32 queueSlot = member->AddBattlegroundQueueId(queueTypeId);

        // send status packet (in queue)
        sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, member, queueSlot, STATUS_WAIT_QUEUE, avgTime, 0, arenaType);
        member->GetSession()->SendPacket(&data);
        sLog->outDebug("Battleground: player joined queue for bg queue type %u bg type %u: GUID %u, NAME %s", queueTypeId, bgTypeId, member->GetGUIDLow(), member->GetName());
    }

    sBattlegroundMgr->ScheduleQueueUpdate(avgRating, arenaType, queueTypeId, bgTypeId, bracketEntry->GetBracketId());
}
