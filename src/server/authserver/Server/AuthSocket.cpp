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

/** \file
    \ingroup realmd
*/

#include "Common.h"
#include "Database/DatabaseEnv.h"
#include "ByteBuffer.h"
#include "Configuration/Config.h"
#include "Log.h"
#include "RealmList.h"
#include "AuthSocket.h"
#include "AuthCodes.h"
#include <openssl/md5.h>
#include "SHA1.h"
//#include "Util.h" -- for commented utf8ToUpperOnlyLatin

#define ChunkSize 2048

// security constant used for avoiding auth server flooding
#define MAX_AUTH_LOGON_CHALLENGES_IN_A_ROW 3

// second flooding exploit constants - flooding in multiple "segments" - allow 30 auth packets per 10 seconds (never sent by normal client)
#define AUTH_PACKET_TIME_TEST_PERIOD 30
#define AUTH_PACKET_PERIOD_LIMIT 10

enum eAuthCmd
{
    AUTH_LOGON_CHALLENGE        = 0x00,
    AUTH_LOGON_PROOF            = 0x01,
    AUTH_RECONNECT_CHALLENGE    = 0x02,
    AUTH_RECONNECT_PROOF        = 0x03,
    REALM_LIST                  = 0x10,
    XFER_INITIATE               = 0x30,
    XFER_DATA                   = 0x31,
    XFER_ACCEPT                 = 0x32,
    XFER_RESUME                 = 0x33,
    XFER_CANCEL                 = 0x34
};

enum eStatus
{
    STATUS_CONNECTED = 0,
    STATUS_AUTHED
};

// GCC have alternative #pragma pack(N) syntax and old gcc version not support pack(push,N), also any gcc version not support it at some paltform
#if defined(__GNUC__)
#pragma pack(1)
#else
#pragma pack(push,1)
#endif

typedef struct AUTH_LOGON_CHALLENGE_C
{
    uint8   cmd;
    uint8   error;
    uint16  size;
    uint8   gamename[4];
    uint8   version1;
    uint8   version2;
    uint8   version3;
    uint16  build;
    uint8   platform[4];
    uint8   os[4];
    uint8   country[4];
    uint32  timezone_bias;
    uint32  ip;
    uint8   I_len;
    uint8   I[1];
} sAuthLogonChallenge_C;

typedef struct AUTH_LOGON_PROOF_C
{
    uint8   cmd;
    uint8   A[32];
    uint8   M1[20];
    uint8   crc_hash[20];
    uint8   number_of_keys;
    uint8   securityFlags;                                  // 0x00-0x04
} sAuthLogonProof_C;

typedef struct AUTH_LOGON_PROOF_S
{
    uint8   cmd;
    uint8   error;
    uint8   M2[20];
    uint32  unk1;
    uint32  unk2;
    uint16  unk3;
} sAuthLogonProof_S;

typedef struct AUTH_LOGON_PROOF_S_OLD
{
    uint8   cmd;
    uint8   error;
    uint8   M2[20];
    //uint32  unk1;
    uint32  unk2;
    //uint16  unk3;
} sAuthLogonProof_S_Old;

typedef struct AUTH_RECONNECT_PROOF_C
{
    uint8   cmd;
    uint8   R1[16];
    uint8   R2[20];
    uint8   R3[20];
    uint8   number_of_keys;
} sAuthReconnectProof_C;

typedef struct XFER_INIT
{
    uint8 cmd;                                              // XFER_INITIATE
    uint8 fileNameLen;                                      // strlen(fileName);
    uint8 fileName[5];                                      // fileName[fileNameLen]
    uint64 file_size;                                       // file size (bytes)
    uint8 md5[MD5_DIGEST_LENGTH];                           // MD5
} XFER_INIT;

typedef struct XFER_DATA
{
    uint8 opcode;
    uint16 data_size;
    uint8 data[ChunkSize];
} XFER_DATA_STRUCT;

typedef struct AuthHandler
{
    eAuthCmd cmd;
    uint32 status;
    bool (AuthSocket::*handler)(void);
} AuthHandler;

// GCC have alternative #pragma pack() syntax and old gcc version not support pack(pop), also any gcc version not support it at some paltform
#if defined(__GNUC__)
#pragma pack()
#else
#pragma pack(pop)
#endif

/// Launch a thread to transfer a patch to the client
class PatcherRunnable: public ACE_Based::Runnable
{
    public:
        PatcherRunnable(class AuthSocket *);
        void run();

    private:
        AuthSocket * mySocket;
};

typedef struct PATCH_INFO
{
    uint8 md5[MD5_DIGEST_LENGTH];
} PATCH_INFO;

/// Caches MD5 hash of client patches present on the server
class Patcher
{
    public:
        typedef std::map<std::string, PATCH_INFO*> Patches;
        ~Patcher();
        Patcher();
        Patches::const_iterator begin() const { return _patches.begin(); }
        Patches::const_iterator end() const { return _patches.end(); }
        void LoadPatchMD5(char*);
        bool GetHash(char * pat,uint8 mymd5[16]);

    private:
        void LoadPatchesInfo();
        Patches _patches;
};

const AuthHandler table[] =
{
    { AUTH_LOGON_CHALLENGE,     STATUS_CONNECTED, &AuthSocket::_HandleLogonChallenge    },
    { AUTH_LOGON_PROOF,         STATUS_CONNECTED, &AuthSocket::_HandleLogonProof        },
    { AUTH_RECONNECT_CHALLENGE, STATUS_CONNECTED, &AuthSocket::_HandleReconnectChallenge},
    { AUTH_RECONNECT_PROOF,     STATUS_CONNECTED, &AuthSocket::_HandleReconnectProof    },
    { REALM_LIST,               STATUS_AUTHED,    &AuthSocket::_HandleRealmList         },
    { XFER_ACCEPT,              STATUS_CONNECTED, &AuthSocket::_HandleXferAccept        },
    { XFER_RESUME,              STATUS_CONNECTED, &AuthSocket::_HandleXferResume        },
    { XFER_CANCEL,              STATUS_CONNECTED, &AuthSocket::_HandleXferCancel        }
};

#define AUTH_TOTAL_COMMANDS sizeof(table)/sizeof(AuthHandler)

///Holds the MD5 hash of client patches present on the server
Patcher PatchesCache;

/// Constructor - set the N and g values for SRP6
AuthSocket::AuthSocket(RealmSocket& socket) : socket_(socket)
{
    N.SetHexStr("894B645E89E1535BBDAD5B8B290650530801B18EBFBF5E8FAB3C82872A3E9BB7");
    g.SetDword(7);
    _authed = false;
    _accountSecurityLevel = SEC_PLAYER;

    _authPacketTime = 0;
    _authPacketCount = 0;
}

/// Close patch file descriptor before leaving
AuthSocket::~AuthSocket(void)
{
}

/// Accept the connection and set the s random value for SRP6
void AuthSocket::OnAccept(void)
{
    sLog->outBasic("Accepting connection from '%s'", socket().get_remote_address().c_str());
}

void AuthSocket::OnClose(void)
{
    sLog->outDebug("AuthSocket::OnClose");
}

/// Read the packet from the client
void AuthSocket::OnRead()
{
    uint32 challengesInARow = 0;

    uint8 _cmd;
    while (1)
    {
        if (!socket().recv_soft((char *)&_cmd, 1))
            return;

        if (_cmd == AUTH_LOGON_CHALLENGE)
        {
            ++challengesInARow;
            if (challengesInARow == MAX_AUTH_LOGON_CHALLENGES_IN_A_ROW)
            {
                sLog->outChar("IP:(%s) Got %u AUTH_LOGON_CHALLENGE in a row, possible ongoing DoS", socket().get_remote_address().c_str(), challengesInARow);
                socket().shutdown();
                return;
            }
        }

        if (_authPacketTime == 0 || _authPacketTime + AUTH_PACKET_TIME_TEST_PERIOD < time(NULL))
        {
            _authPacketTime = time(NULL);
            _authPacketCount = 1;
        }
        else
        {
            _authPacketCount++;
            _authPacketTime = time(NULL);

            if (_authPacketCount > AUTH_PACKET_PERIOD_LIMIT)
            {
                sLog->outChar("IP:(%s) Got %u auth packets in a row, possible ongoing DoS (second type auth flooding exploit)", socket().get_remote_address().c_str(), challengesInARow);
                socket().shutdown();
                return;
            }
        }

        size_t i;

        ///- Circle through known commands and call the correct command handler
        for (i = 0; i < AUTH_TOTAL_COMMANDS; ++i)
        {
            if ((uint8)table[i].cmd == _cmd &&
                (table[i].status == STATUS_CONNECTED ||
                (_authed && table[i].status == STATUS_AUTHED)))
            {
                sLog->outStaticDebug("[Auth] got data for cmd %u recv length %u", (uint32)_cmd, (uint32)socket().recv_len());

                if (!(*this.*table[i].handler)())
                {
                    sLog->outStaticDebug("Command handler failed for cmd %u recv length %u", (uint32)_cmd, (uint32)socket().recv_len());
                    return;
                }
                break;
            }
        }

        // Report unknown packets in the error log
        if (i == AUTH_TOTAL_COMMANDS)
        {
            sLog->outError("[Auth] got unknown packet from '%s'", socket().get_remote_address().c_str());
            socket().shutdown();
            return;
        }
    }
}

/// Make the SRP6 calculation from hash in dB
void AuthSocket::_SetVSFields(const std::string& rI)
{
    s.SetRand(s_BYTE_SIZE * 8);

    BigNumber I;
    I.SetHexStr(rI.c_str());

    // In case of leading zeros in the rI hash, restore them
    uint8 mDigest[SHA_DIGEST_LENGTH];
    memset(mDigest, 0, SHA_DIGEST_LENGTH);
    if (I.GetNumBytes() <= SHA_DIGEST_LENGTH)
        memcpy(mDigest, I.AsByteArray(), I.GetNumBytes());

    std::reverse(mDigest, mDigest + SHA_DIGEST_LENGTH);

    SHA1Hash sha;
    sha.UpdateData(s.AsByteArray(), s.GetNumBytes());
    sha.UpdateData(mDigest, SHA_DIGEST_LENGTH);
    sha.Finalize();
    BigNumber x;
    x.SetBinary(sha.GetDigest(), sha.GetLength());
    v = g.ModExp(x, N);
    // No SQL injection (username escaped)
    const char *v_hex, *s_hex;
    v_hex = v.AsHexStr();
    s_hex = s.AsHexStr();

    PreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SET_VS);
    stmt->setString(0, v_hex);
    stmt->setString(1, s_hex);
    stmt->setString(2, _login);
    LoginDatabase.Execute(stmt);

    OPENSSL_free((void*)v_hex);
    OPENSSL_free((void*)s_hex);
}

/// Logon Challenge command handler
bool AuthSocket::_HandleLogonChallenge()
{
    sLog->outStaticDebug("Entering _HandleLogonChallenge");
    if (socket().recv_len() < sizeof(sAuthLogonChallenge_C))
        return false;

    ///- Read the first 4 bytes (header) to get the length of the remaining of the packet
    std::vector<uint8> buf;
    buf.resize(4);

    socket().recv((char *)&buf[0], 4);
#if TRINITY_ENDIAN == TRINITY_BIGENDIAN
    EndianConvert(*((uint16*)(buf[0])));
#endif
    uint16 remaining = ((sAuthLogonChallenge_C *)&buf[0])->size;
    sLog->outStaticDebug("[AuthChallenge] got header, body is %#04x bytes", remaining);

    if ((remaining < sizeof(sAuthLogonChallenge_C) - buf.size()) || (socket().recv_len() < remaining))
        return false;

    //No big fear of memory outage (size is int16, i.e. < 65536)
    buf.resize(remaining + buf.size() + 1);
    buf[buf.size() - 1] = 0;
    sAuthLogonChallenge_C *ch = (sAuthLogonChallenge_C*)&buf[0];

    ///- Read the remaining of the packet
    socket().recv((char *)&buf[4], remaining);
    sLog->outStaticDebug("[AuthChallenge] got full packet, %#04x bytes", ch->size);
    sLog->outStaticDebug("[AuthChallenge] name(%d): '%s'", ch->I_len, ch->I);

#if TRINITY_ENDIAN == TRINITY_BIGENDIAN
    // BigEndian code, nop in little endian case
    // size already converted
    EndianConvert(*((uint32*)(&ch->gamename[0])));
    EndianConvert(ch->build);
    EndianConvert(*((uint32*)(&ch->platform[0])));
    EndianConvert(*((uint32*)(&ch->os[0])));
    EndianConvert(*((uint32*)(&ch->country[0])));
    EndianConvert(ch->timezone_bias);
    EndianConvert(ch->ip);
#endif

    ByteBuffer pkt;

    _login = (const char*)ch->I;
    _build = ch->build;
    _expversion = (AuthHelper::IsPostWotLKAcceptedClientBuild(_build) ? POST_WOTLK_EXP_FLAG : NO_VALID_EXP_FLAG) |
                  (AuthHelper::IsPostBCAcceptedClientBuild(_build) ? POST_BC_EXP_FLAG : NO_VALID_EXP_FLAG) |
                  (AuthHelper::IsPreBCAcceptedClientBuild(_build) ? PRE_BC_EXP_FLAG : NO_VALID_EXP_FLAG);

    _build = ch->build;

    pkt << (uint8) AUTH_LOGON_CHALLENGE;
    pkt << (uint8) 0x00;

    // Verify that this IP is not in the ip_banned table
    LoginDatabase.Execute(LoginDatabase.GetPreparedStatement(LOGIN_SET_EXPIREDIPBANS));

    const std::string& ip_address = socket().get_remote_address();
    PreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_GET_IPBANNED);
    stmt->setString(0, ip_address);
    PreparedQueryResult result = LoginDatabase.Query(stmt);
    if (result)
    {
        pkt << (uint8)WOW_FAIL_BANNED;
        sLog->outBasic("[AuthChallenge] Banned ip %s tries to login!", ip_address.c_str());
    }
    else
    {
        ///- Get the account details from the account table
        stmt = LoginDatabase.GetPreparedStatement(LOGIN_GET_LOGONCHALLENGE);
        stmt->setString(0, _login);

        PreparedQueryResult res2 = LoginDatabase.Query(stmt);
        if (res2)
        {
            Field* fields = res2->Fetch();

            //- If the IP is 'locked', check that the player comes indeed from the correct IP address
            bool locked = false;
            if (fields[2].GetUInt8() == 1)            // if ip is locked
            {
                sLog->outStaticDebug("[AuthChallenge] Account '%s' is locked to IP - '%s'", _login.c_str(), fields[3].GetCString());
                sLog->outStaticDebug("[AuthChallenge] Player address is '%s'", ip_address.c_str());
                if (strcmp(fields[3].GetCString(), ip_address.c_str()))
                {
                    sLog->outStaticDebug("[AuthChallenge] Account IP differs");
                    pkt << (uint8) WOW_FAIL_SUSPENDED;
                    locked=true;
                }
                else
                    sLog->outStaticDebug("[AuthChallenge] Account IP matches");
            }
            else
                sLog->outStaticDebug("[AuthChallenge] Account '%s' is not locked to ip", _login.c_str());

            if (!locked)
            {
                //set expired bans to inactive
                LoginDatabase.Execute(LoginDatabase.GetPreparedStatement(LOGIN_SET_EXPIREDACCBANS));

                // If the account is banned, reject the logon attempt
                stmt = LoginDatabase.GetPreparedStatement(LOGIN_GET_ACCBANNED);
                stmt->setUInt32(0, fields[1].GetUInt32());
                PreparedQueryResult banresult = LoginDatabase.Query(stmt);
                if (banresult)
                {
                    if ((*banresult)[0].GetUInt64() == (*banresult)[1].GetUInt64())
                    {
                        pkt << (uint8) WOW_FAIL_BANNED;
                        sLog->outBasic("[AuthChallenge] Banned account %s tries to login!", _login.c_str());
                    }
                    else
                    {
                        pkt << (uint8) WOW_FAIL_SUSPENDED;
                        sLog->outBasic("[AuthChallenge] Temporarily banned account %s tries to login!", _login.c_str());
                    }
                }
                else
                {
                    ///- Get the password from the account table, upper it, and make the SRP6 calculation
                    std::string rI = fields[0].GetString();

                    ///- Don't calculate (v, s) if there are already some in the database
                    std::string databaseV = fields[5].GetString();
                    std::string databaseS = fields[6].GetString();

                    sLog->outDebug("database authentication values: v='%s' s='%s'", databaseV.c_str(), databaseS.c_str());

                    // multiply with 2, bytes are stored as hexstring
                    if (databaseV.size() != s_BYTE_SIZE*2 || databaseS.size() != s_BYTE_SIZE*2)
                        _SetVSFields(rI);
                    else
                    {
                        s.SetHexStr(databaseS.c_str());
                        v.SetHexStr(databaseV.c_str());
                    }

                    b.SetRand(19 * 8);
                    BigNumber gmod = g.ModExp(b, N);
                    B = ((v * 3) + gmod) % N;

                    ASSERT(gmod.GetNumBytes() <= 32);

                    BigNumber unk3;
                    unk3.SetRand(16 * 8);

                    ///- Fill the response packet with the result
                    pkt << uint8(WOW_SUCCESS);

                    // B may be calculated < 32B so we force minimal length to 32B
                    pkt.append(B.AsByteArray(32), 32);      // 32 bytes
                    pkt << uint8(1);
                    pkt.append(g.AsByteArray(), 1);
                    pkt << uint8(32);
                    pkt.append(N.AsByteArray(32), 32);
                    pkt.append(s.AsByteArray(), s.GetNumBytes());   // 32 bytes
                    pkt.append(unk3.AsByteArray(16), 16);
                    uint8 securityFlags = 0;
                    pkt << uint8(securityFlags);            // security flags (0x0...0x04)

                    if (securityFlags & 0x01)                // PIN input
                    {
                        pkt << uint32(0);
                        pkt << uint64(0) << uint64(0);      // 16 bytes hash?
                    }

                    if (securityFlags & 0x02)                // Matrix input
                    {
                        pkt << uint8(0);
                        pkt << uint8(0);
                        pkt << uint8(0);
                        pkt << uint8(0);
                        pkt << uint64(0);
                    }

                    if (securityFlags & 0x04)                // Security token input
                        pkt << uint8(1);

                    uint8 secLevel = fields[4].GetUInt8();
                    _accountSecurityLevel = secLevel <= SEC_ADMINISTRATOR ? AccountTypes(secLevel) : SEC_ADMINISTRATOR;

                    _localizationName.resize(4);
                    for (int i = 0; i < 4; ++i)
                        _localizationName[i] = ch->country[4-i-1];

                    sLog->outBasic("[AuthChallenge] account %s is using '%c%c%c%c' locale (%u)", _login.c_str (), ch->country[3], ch->country[2], ch->country[1], ch->country[0], GetLocaleByName(_localizationName));
                }
            }
        }
        else                                            //no account
            pkt << (uint8)WOW_FAIL_UNKNOWN_ACCOUNT;
    }

    socket().send((char const*)pkt.contents(), pkt.size());
    return true;
}

/// Logon Proof command handler
bool AuthSocket::_HandleLogonProof()
{
    sLog->outStaticDebug("Entering _HandleLogonProof");
    // Read the packet
    sAuthLogonProof_C lp;

    if (!socket().recv((char *)&lp, sizeof(sAuthLogonProof_C)))
        return false;

    // If the client has no valid version
    if (_expversion == NO_VALID_EXP_FLAG)
    {
        // Check if we have the appropriate patch on the disk

        sLog->outDebug("Client with invalid version, patching is not implemented");
        socket().shutdown();
        return true;
    }

    // Continue the SRP6 calculation based on data received from the client
    BigNumber A;

    A.SetBinary(lp.A, 32);

    // SRP safeguard: abort if A == 0
    if (A.isZero())
    {
        socket().shutdown();
        return true;
    }

    SHA1Hash sha;
    sha.UpdateBigNumbers(&A, &B, NULL);
    sha.Finalize();
    BigNumber u;
    u.SetBinary(sha.GetDigest(), 20);
    BigNumber S = (A * (v.ModExp(u, N))).ModExp(b, N);

    uint8 t[32];
    uint8 t1[16];
    uint8 vK[40];
    memcpy(t, S.AsByteArray(32), 32);
    for (int i = 0; i < 16; ++i)
    {
        t1[i] = t[i * 2];
    }
    sha.Initialize();
    sha.UpdateData(t1, 16);
    sha.Finalize();
    for (int i = 0; i < 20; ++i)
        vK[i * 2] = sha.GetDigest()[i];
    for (int i = 0; i < 16; ++i)
        t1[i] = t[i * 2 + 1];
    sha.Initialize();
    sha.UpdateData(t1, 16);
    sha.Finalize();
    for (int i = 0; i < 20; ++i)
        vK[i * 2 + 1] = sha.GetDigest()[i];
    K.SetBinary(vK, 40);

    uint8 hash[20];

    sha.Initialize();
    sha.UpdateBigNumbers(&N, NULL);
    sha.Finalize();
    memcpy(hash, sha.GetDigest(), 20);
    sha.Initialize();
    sha.UpdateBigNumbers(&g, NULL);
    sha.Finalize();
    for (int i = 0; i < 20; ++i)
        hash[i] ^= sha.GetDigest()[i];
    BigNumber t3;
    t3.SetBinary(hash, 20);

    sha.Initialize();
    sha.UpdateData(_login);
    sha.Finalize();
    uint8 t4[SHA_DIGEST_LENGTH];
    memcpy(t4, sha.GetDigest(), SHA_DIGEST_LENGTH);

    sha.Initialize();
    sha.UpdateBigNumbers(&t3, NULL);
    sha.UpdateData(t4, SHA_DIGEST_LENGTH);
    sha.UpdateBigNumbers(&s, &A, &B, &K, NULL);
    sha.Finalize();
    BigNumber M;
    M.SetBinary(sha.GetDigest(), 20);

    // Check if SRP6 results match (password is correct), else send an error
    if (!memcmp(M.AsByteArray(), lp.M1, 20))
    {
        sLog->outBasic("User '%s' successfully authenticated", _login.c_str());

        // Update the sessionkey, last_ip, last login time and reset number of failed logins in the account table for this account
        const char* K_hex = K.AsHexStr();

        PreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SET_LOGONPROOF);
        stmt->setString(0, K_hex);
        stmt->setString(1, socket().get_remote_address().c_str());
        stmt->setUInt32(2, GetLocaleByName(_localizationName));
        stmt->setString(3, _login);
        LoginDatabase.Execute(stmt);

        OPENSSL_free((void*)K_hex);

        // Finish SRP6 and send the final result to the client
        sha.Initialize();
        sha.UpdateBigNumbers(&A, &M, &K, NULL);
        sha.Finalize();

        if ((_expversion & POST_BC_EXP_FLAG) || (_expversion & POST_WOTLK_EXP_FLAG))
        {
            sAuthLogonProof_S proof;
            memcpy(proof.M2, sha.GetDigest(), 20);
            proof.cmd = AUTH_LOGON_PROOF;
            proof.error = 0;
            proof.unk1 = 0x00800000;
            proof.unk2 = 0x00;
            proof.unk3 = 0x00;
            socket().send((char *)&proof, sizeof(proof));
        }
        else
        {
            sAuthLogonProof_S_Old proof;
            memcpy(proof.M2, sha.GetDigest(), 20);
            proof.cmd = AUTH_LOGON_PROOF;
            proof.error = 0;
            proof.unk2 = 0x00;
            socket().send((char *)&proof, sizeof(proof));
        }

        _authed = true;
    }
    else
    {
        char data[4]= { AUTH_LOGON_PROOF, WOW_FAIL_UNKNOWN_ACCOUNT, 3, 0};
        socket().send(data, sizeof(data));
        
        sLog->outBasic("[AuthChallenge] account %s tried to login with wrong password!",_login.c_str ());

        uint32 MaxWrongPassCount = sConfig->GetIntDefault("WrongPass.MaxCount", 0);
        if (MaxWrongPassCount > 0)
        {
            // Increment number of failed logins by one and if it reaches the limit temporarily ban that account or IP
            PreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SET_FAILEDLOGINS);
            stmt->setString(0, _login);
            LoginDatabase.Execute(stmt);

            stmt = LoginDatabase.GetPreparedStatement(LOGIN_GET_FAILEDLOGINS);
            stmt->setString(0, _login);

            if (PreparedQueryResult loginfail = LoginDatabase.Query(stmt))
            {
                uint32 failed_logins = (*loginfail)[1].GetUInt32();

                if (failed_logins >= MaxWrongPassCount)
                {
                    uint32 WrongPassBanTime = sConfig->GetIntDefault("WrongPass.BanTime", 600);
                    bool WrongPassBanType = sConfig->GetBoolDefault("WrongPass.BanType", false);

                    if (WrongPassBanType)
                    {
                        uint32 acc_id = (*loginfail)[0].GetUInt32();
                        stmt = LoginDatabase.GetPreparedStatement(LOGIN_SET_ACCAUTOBANNED);
                        stmt->setUInt32(0, acc_id);
                        stmt->setUInt32(1, WrongPassBanTime);
                        LoginDatabase.Execute(stmt);

                        sLog->outBasic("[AuthChallenge] account %s got banned for '%u' seconds because it failed to authenticate '%u' times",
                            _login.c_str(), WrongPassBanTime, failed_logins);
                    }
                    else
                    {
                        stmt = LoginDatabase.GetPreparedStatement(LOGIN_SET_IPAUTOBANNED);
                        stmt->setString(0, socket().get_remote_address());
                        stmt->setUInt32(1, WrongPassBanTime);
                        LoginDatabase.Execute(stmt);

                        sLog->outBasic("[AuthChallenge] IP %s got banned for '%u' seconds because account %s failed to authenticate '%u' times",
                            socket().get_remote_address().c_str(), WrongPassBanTime, _login.c_str(), failed_logins);
                    }
                }
            }
        }
    }

    return true;
}

/// Reconnect Challenge command handler
bool AuthSocket::_HandleReconnectChallenge()
{
    sLog->outStaticDebug("Entering _HandleReconnectChallenge");
    if (socket().recv_len() < sizeof(sAuthLogonChallenge_C))
        return false;

    // Read the first 4 bytes (header) to get the length of the remaining of the packet
    std::vector<uint8> buf;
    buf.resize(4);

    socket().recv((char *)&buf[0], 4);

#if TRINITY_ENDIAN == TRINITY_BIGENDIAN
    EndianConvert(*((uint16*)(buf[0])));
#endif //TRINITY_ENDIAN
    uint16 remaining = ((sAuthLogonChallenge_C *)&buf[0])->size;
    sLog->outStaticDebug("[ReconnectChallenge] got header, body is %#04x bytes", remaining);

    if ((remaining < sizeof(sAuthLogonChallenge_C) - buf.size()) || (socket().recv_len() < remaining))
        return false;

    // No big fear of memory outage (size is int16, i.e. < 65536)
    buf.resize(remaining + buf.size() + 1);
    buf[buf.size() - 1] = 0;
    sAuthLogonChallenge_C *ch = (sAuthLogonChallenge_C*)&buf[0];

    // Read the remaining of the packet
    socket().recv((char *)&buf[4], remaining);
    sLog->outStaticDebug("[ReconnectChallenge] got full packet, %#04x bytes", ch->size);
    sLog->outStaticDebug("[ReconnectChallenge] name(%d): '%s'", ch->I_len, ch->I);

    _login = (const char*)ch->I;

    PreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_GET_SESSIONKEY);
    stmt->setString(0, _login);
    PreparedQueryResult result = LoginDatabase.Query(stmt);

    // Stop if the account is not found
    if (!result)
    {
        sLog->outError("[ERROR] user %s tried to login and we cannot find his session key in the database.", _login.c_str());
        socket().shutdown();
        return false;
    }

    K.SetHexStr ((*result)[0].GetCString());

    // Sending response
    ByteBuffer pkt;
    pkt << (uint8)AUTH_RECONNECT_CHALLENGE;
    pkt << (uint8)0x00;
    _reconnectProof.SetRand(16 * 8);
    pkt.append(_reconnectProof.AsByteArray(16), 16);             // 16 bytes random
    pkt << (uint64)0x00 << (uint64)0x00;                  // 16 bytes zeros
    socket().send((char const*)pkt.contents(), pkt.size());
    return true;
}

/// Reconnect Proof command handler
bool AuthSocket::_HandleReconnectProof()
{
    sLog->outStaticDebug("Entering _HandleReconnectProof");
    // Read the packet
    sAuthReconnectProof_C lp;
    if (!socket().recv((char *)&lp, sizeof(sAuthReconnectProof_C)))
        return false;

    if (_login.empty() || !_reconnectProof.GetNumBytes() || !K.GetNumBytes())
        return false;

    BigNumber t1;
    t1.SetBinary(lp.R1, 16);

    SHA1Hash sha;
    sha.Initialize();
    sha.UpdateData(_login);
    sha.UpdateBigNumbers(&t1, &_reconnectProof, &K, NULL);
    sha.Finalize();

    if (!memcmp(sha.GetDigest(), lp.R2, SHA_DIGEST_LENGTH))
    {
        // Sending response
        ByteBuffer pkt;
        pkt << (uint8)  AUTH_RECONNECT_PROOF;
        pkt << (uint8)  0x00;
        pkt << (uint16) 0x00;                               // 2 bytes zeros
        socket().send((char const*)pkt.contents(), pkt.size());

        _authed = true;

        return true;
    }
    else
    {
        sLog->outError("[ERROR] user %s tried to login, but session invalid.", _login.c_str());
        socket().shutdown();
        return false;
    }
}

/// %Realm List command handler
bool AuthSocket::_HandleRealmList()
{
    sLog->outStaticDebug("Entering _HandleRealmList");
    if (socket().recv_len() < 5)
        return false;

    socket().recv_skip(5);

    // Get the user id (else close the connection)
    PreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_GET_ACCIDBYNAME);
    stmt->setString(0, _login);
    PreparedQueryResult result = LoginDatabase.Query(stmt);
    if (!result)
    {
        sLog->outError("[ERROR] user %s tried to login and we cannot find him in the database.", _login.c_str());
        socket().shutdown();
        return false;
    }

    Field* fields = result->Fetch();
    uint32 id = fields[0].GetUInt32();

    // Update realm list if need
    sRealmList->UpdateIfNeed();

    // Circle through realms in the RealmList and construct the return packet (including # of user characters in each realm)
    ByteBuffer pkt;

    size_t RealmListSize = 0;
    for (RealmList::RealmMap::const_iterator i = sRealmList->begin(); i != sRealmList->end(); ++i)
    {
        // don't work with realms which not compatible with the client
        if ((_expversion & POST_BC_EXP_FLAG) || (_expversion & POST_WOTLK_EXP_FLAG))
        {
            if (i->second.gamebuild != _build)
            {
                sLog->outStaticDebug("Realm not added because of not correct build : %u != %u", i->second.gamebuild, _build);
                continue;
            }
        }
        else if (_expversion & PRE_BC_EXP_FLAG) // 1.12.1 and 1.12.2 clients are compatible with eachother
            if (!AuthHelper::IsPreBCAcceptedClientBuild(i->second.gamebuild))
                continue;

        uint8 AmountOfCharacters;

        stmt = LoginDatabase.GetPreparedStatement(LOGIN_GET_NUMCHARSONREALM);
        stmt->setUInt32(0, i->second.m_ID);
        stmt->setUInt32(1, id);
        result = LoginDatabase.Query(stmt);
        if (result)
            AmountOfCharacters = (*result)[0].GetUInt8();
        else
            AmountOfCharacters = 0;

        uint8 lock = (i->second.allowedSecurityLevel > _accountSecurityLevel) ? 1 : 0;

        pkt << i->second.icon;
        if (_expversion & (POST_BC_EXP_FLAG | POST_WOTLK_EXP_FLAG))
            pkt << lock;
        pkt << i->second.color;
        pkt << i->first;

        /* if client's remote address is in the same /16 subnet
         * ("172.27." = 7 chars) as the server's address,
         * return the internal server address */
        /* PS: don't fuck with std::string.compare(), it fights back too hard */
        std::string gmvpn_server = "172.27.0.1";
        gmvpn_server.append(strchr(i->second.address.c_str(),':'));  // append original port
        if (memcmp(socket().get_remote_address().c_str(), gmvpn_server.c_str(), 6)==0)
            pkt << gmvpn_server;
        else
            pkt << i->second.address;

        pkt << i->second.populationLevel;
        pkt << AmountOfCharacters;
        pkt << i->second.timezone;
        if (_expversion & (POST_BC_EXP_FLAG | POST_WOTLK_EXP_FLAG))
            pkt << (uint8) 0x2C;
        else
            pkt << (uint8) 0x0;

        ++RealmListSize;
    }

    if ((_expversion & POST_BC_EXP_FLAG) || (_expversion & POST_WOTLK_EXP_FLAG))
    {
        pkt << (uint8) 0x10;
        pkt << (uint8) 0x00;
    }
    else
    {
        pkt << (uint8) 0x00;
        pkt << (uint8) 0x02;
    }

    // make a ByteBuffer which stores the RealmList's size
    ByteBuffer RealmListSizeBuffer;
    RealmListSizeBuffer << (uint32)0;
    if ((_expversion & POST_BC_EXP_FLAG) || (_expversion & POST_WOTLK_EXP_FLAG))
        RealmListSizeBuffer << (uint16)RealmListSize;
    else
        RealmListSizeBuffer << (uint32)RealmListSize;

    ByteBuffer hdr;
    hdr << (uint8) REALM_LIST;
    hdr << (uint16)(pkt.size() + RealmListSizeBuffer.size());
    hdr.append(RealmListSizeBuffer);    // append RealmList's size buffer
    hdr.append(pkt);                    // append realms in the realmlist

    socket().send((char const*)hdr.contents(), hdr.size());

    return true;
}

/// Resume patch transfer
bool AuthSocket::_HandleXferResume()
{
    sLog->outStaticDebug("Entering _HandleXferResume");
    // Check packet length and patch existence
    if (socket().recv_len() < 9 || !pPatch)
    {
        sLog->outError("Error while resuming patch transfer (wrong packet)");
        return false;
    }

    // Launch a PatcherRunnable thread starting at given patch file offset
    uint64 start;
    socket().recv_skip(1);
    socket().recv((char*)&start,sizeof(start));
    fseek(pPatch, long(start), 0);

    ACE_Based::Thread u(new PatcherRunnable(this));
    return true;
}

/// Cancel patch transfer
bool AuthSocket::_HandleXferCancel()
{
    sLog->outStaticDebug("Entering _HandleXferCancel");

    // Close and delete the socket
    socket().recv_skip(1);                                         //clear input buffer

    socket().shutdown();

    return true;
}

/// Accept patch transfer
bool AuthSocket::_HandleXferAccept()
{
    sLog->outStaticDebug("Entering _HandleXferAccept");

    // Check packet length and patch existence
    if (!pPatch)
    {
        sLog->outError("Error while accepting patch transfer (wrong packet)");
        return false;
    }

    // Launch a PatcherRunnable thread, starting at the beginning of the patch file
    socket().recv_skip(1);                                         // clear input buffer
    fseek(pPatch, 0, 0);

    ACE_Based::Thread u(new PatcherRunnable(this));
    return true;
}

PatcherRunnable::PatcherRunnable(class AuthSocket * as)
{
    mySocket = as;
}

/// Send content of patch file to the client
void PatcherRunnable::run()
{
}

/// Preload MD5 hashes of existing patch files on server
#ifndef _WIN32
#include <dirent.h>
#include <errno.h>
void Patcher::LoadPatchesInfo()
{
    DIR * dirp;
    //int errno;
    struct dirent * dp;
    dirp = opendir("./patches/");
    if (!dirp)
        return;
    while (dirp)
    {
        errno = 0;
        if ((dp = readdir(dirp)) != NULL)
        {
            int l = strlen(dp->d_name);
            if (l < 8)
                continue;
            if (!memcmp(&dp->d_name[l-4],".mpq",4))
                LoadPatchMD5(dp->d_name);
        }
        else
        {
            if (errno != 0)
            {
                closedir(dirp);
                return;
            }
            break;
        }
    }

    if (dirp)
        closedir(dirp);
}

#else
void Patcher::LoadPatchesInfo()
{
    WIN32_FIND_DATA fil;
    HANDLE hFil=FindFirstFile("./patches/*.mpq", &fil);
    if (hFil == INVALID_HANDLE_VALUE)
        return;                                             // no patches were found

    do
    {
        LoadPatchMD5(fil.cFileName);
    }
    while(FindNextFile(hFil, &fil));
}
#endif

/// Calculate and store MD5 hash for a given patch file
void Patcher::LoadPatchMD5(char * szFileName)
{
    ///- Try to open the patch file
    std::string path = "./patches/";
    path += szFileName;
    FILE *pPatch = fopen(path.c_str(), "rb");
    sLog->outDebug("Loading patch info from %s\n", path.c_str());
    if (!pPatch)
    {
        sLog->outError("Error loading patch %s\n", path.c_str());
        return;
    }

    // Calculate the MD5 hash
    MD5_CTX ctx;
    MD5_Init(&ctx);
    uint8* buf = new uint8[512*1024];

    while (!feof(pPatch))
    {
        size_t read = fread(buf, 1, 512*1024, pPatch);
        MD5_Update(&ctx, buf, read);
    }
    delete [] buf;
    fclose(pPatch);

    // Store the result in the internal patch hash map
    _patches[path] = new PATCH_INFO;
    MD5_Final((uint8 *)&_patches[path]->md5, &ctx);
}

/// Get cached MD5 hash for a given patch file
bool Patcher::GetHash(char * pat, uint8 mymd5[16])
{
    for (Patches::iterator i = _patches.begin(); i != _patches.end(); ++i)
        if (!stricmp(pat, i->first.c_str()))
    {
        memcpy(mymd5, i->second->md5, 16);
        return true;
    }

    return false;
}

/// Launch the patch hashing mechanism on object creation
Patcher::Patcher()
{
    LoadPatchesInfo();
}

/// Empty and delete the patch map on termination
Patcher::~Patcher()
{
    for (Patches::iterator i = _patches.begin(); i != _patches.end(); ++i)
        delete i->second;
}
