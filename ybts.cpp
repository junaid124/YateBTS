/**
 * ybts.cpp
 * This file is part of the Yate-BTS Project http://www.yatebts.com
 *
 * Yet Another BTS Channel
 *
 * Yet Another Telephony Engine - Base Transceiver Station
 * Copyright (C) 2013-2014 Null Team Impex SRL
 * Copyright (C) 2014 Legba, Inc
 *
 * This software is distributed under multiple licenses;
 * see the COPYING file in the main directory for licensing
 * information for this specific distribution.
 *
 * This use of this software may be subject to additional restrictions.
 * See the LEGAL file in the main directory for details.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 */

#include <yatephone.h>
#include <yategsm.h>

#ifdef _WINDOWS
#error This module is not for Windows
#endif

#include <sys/stat.h>
#include <sys/wait.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <syslog.h>

using namespace TelEngine;
namespace { // anonymous

#include "ybts.h"

#define BTS_DIR "bts"
#define BTS_CMD "mbts"

// Handshake interval (timeout)
#define YBTS_HK_INTERVAL_DEF 60000
#define YBTS_HK_INTERVAL_MIN 20000
#define YBTS_HK_INTERVAL_MAX 300000
// Heartbeat interval
#define YBTS_HB_INTERVAL_DEF 30000
#define YBTS_HB_INTERVAL_MIN 1000
#define YBTS_HB_INTERVAL_MAX 120000
// Heartbeat timeout
#define YBTS_HB_TIMEOUT_DEF 60000
#define YBTS_HB_TIMEOUT_MIN 10000
#define YBTS_HB_TIMEOUT_MAX 180000
// Restart
#define YBTS_RESTART_DEF 120000
#define YBTS_RESTART_MIN 30000
#define YBTS_RESTART_MAX 600000
#define YBTS_RESTART_COUNT_DEF 10
#define YBTS_RESTART_COUNT_MIN 3
// Peer check
#define YBTS_PEERCHECK_DEF 3000
// SMS
#define YBTS_MT_SMS_TIMEOUT_DEF 300000
#define YBTS_MT_SMS_TIMEOUT_MIN 5000
#define YBTS_MT_SMS_TIMEOUT_MAX 600000
// USSD
#define YBTS_USSD_TIMEOUT_DEF 600000
#define YBTS_USSD_TIMEOUT_MIN 30000

#define YBTS_SET_REASON_BREAK(s) { reason = s; break; }

// Constant strings
static const String s_message = "Message";
static const String s_type = "type";
static const String s_locAreaIdent = "LAI";
static const String s_PLMNidentity = "PLMNidentity";
static const String s_LAC = "LAC";
static const String s_mobileIdent = "MobileIdentity";
static const String s_imsi = "IMSI";
static const String s_tmsi = "TMSI";
static const String s_cause = "Cause";
static const String s_causeLocation = "location";
static const String s_causeCoding = "coding";
static const String s_cmServType = "CMServiceType";
static const String s_cmMOCall = "MO-call-establishment-or-PM-connection-establishment";
static const String s_cmSMS = "SMS";
static const String s_cmSS = "SS-activation";
static const String s_facility = "Facility";
static const String s_rlc = "ReleaseComplete";
static const String s_ccSetup = "Setup";
static const String s_ccEmergency = "EmergencySetup";
static const String s_ccProceeding = "CallProceeding";
static const String s_ccProgress = "Progress";
static const String s_ccAlerting = "Alerting";
static const String s_ccConnect = "Connect";
static const String s_ccConnectAck = "ConnectAcknowledge";
static const String s_ccDisc = "Disconnect";
static const String s_ccRel = "Release";
static const String s_ccStatusEnq = "StatusEnquiry";
static const String s_ccStatus = "Status";
static const String s_ccStartDTMF = "StartDTMF";
static const String s_ccStopDTMF = "StopDTMF";
static const String s_ccKeypadFacility = "KeypadFacility";
static const String s_ccHold = "Hold";
static const String s_ccRetrieve = "Retrieve";
static const String s_ccConfirmed = "CallConfirmed";
static const String s_ccCallRef = "TID";
static const String s_ccTIFlag = "TIFlag";
static const String s_ccCalled = "CalledPartyBCDNumber";
static const String s_ccCallState = "CallState";
static const String s_ccProgressInd = "ProgressIndicator";
//static const String s_ccUserUser = "UserUser";
static const String s_ccSsCLIR = "CLIRInvocation";
static const String s_ccSsCLIP = "CLIRSuppresion";
// MAP related
static const String s_mapComp = "component";
static const String s_mapLocalCID = "localCID";
static const String s_mapRemoteCID = "remoteCID";
static const String s_mapCompType = "type";
static const String s_mapOperCode = "operationCode";
static const String s_mapUssdText = "ussd-Text";
// Global
static const String s_error = "error";

class YBTSConnIdHolder;                  // A connection id holder
class YBTSThread;
class YBTSThreadOwner;
class YBTSMessage;                       // YBTS <-> BTS PDU
class YBTSTransport;
class YBTSGlobalThread;                  // GenObject and Thread descendent
class YBTSLAI;                           // Holds local area id
class YBTSTid;                           // Transaction identifier holder
class YBTSConn;                          // A logical connection
class YBTSLog;                           // Log interface
class YBTSCommand;                       // Command interface
class YBTSSignalling;                    // Signalling interface
class YBTSMedia;                         // Media interface
class YBTSUE;                            // A registered equipment
class YBTSLocationUpd;                   // Running location update from UE
class YBTSSubmit;                        // MO SMS/SS submit thread
class YBTSSmsInfo;                       // Holds data describing a pending SMS
class YBTSMtSms;                         // Holds data describing a pending MT SMS
class YBTSMtSmsList;                     // A list of MT SMS for the same UE target
class YBTSMM;                            // Mobility management entity
class YBTSDataSource;
class YBTSDataConsumer;
class YBTSCallDesc;
class YBTSChan;
class YBTSDriver;
class YBTSMsgHandler;

// ETSI TS 100 940
// Section 8.5 / Section 10.5.3.6 / Annex G
enum RejectCause {
    CauseServNotSupp = 32,               // Service option not implemented
    CauseInvalidIE = 96,                 // Invalid mandatory IE
    CauseUnknownMsg = 97,                // Unknown or not implemented message
    CauseUnexpectedMsg = 98,             // Message not compatible with protocol state
    CauseProtoError = 111,               // Protocol error, unspecified
};

static inline int str2index(const String& name, const String* array)
{
    if (!array)
	return -1;
    for (int i = 0; *array; i++, array++)
	if (*array == name)
	    return i;
    return -1;
}

static inline const String& index2str(int index, const String* array)
{
    if (!array || index < 0)
	return String::empty();
    for (int i = 0; *array; i++, array++)
	if (i == index)
	    return *array;
    return String::empty();
}

class YBTSConnIdHolder
{
public:
    inline YBTSConnIdHolder(uint16_t connId = 0)
	: m_connId(connId)
	{}
    inline uint16_t connId() const
	{ return m_connId; }
    inline void setConnId(uint16_t cid)
	{ m_connId = cid; }
protected:
    uint16_t m_connId;
};

class YBTSDebug
{
public:
    inline YBTSDebug(DebugEnabler* enabler = 0, void* ptr = 0)
	{ setDebugPtr(enabler,ptr); }
    inline void setDebugPtr(DebugEnabler* enabler, void* ptr) {
	    m_enabler = enabler;
	    m_ptr = ptr;
	}
protected:
    DebugEnabler* m_enabler;
    void* m_ptr;
};

class YBTSThread : public Thread
{
public:
    YBTSThread(YBTSThreadOwner* owner, const char* name, Thread::Priority prio = Thread::Normal);
    virtual void cleanup();
protected:
    virtual void run();
    void notifyTerminate();
    YBTSThreadOwner* m_owner;
};

class YBTSThreadOwner : public YBTSDebug
{
public:
    inline YBTSThreadOwner()
	: m_thread(0), m_mutex(0)
	{}
    void threadTerminated(YBTSThread* th);
    virtual void processLoop() = 0;

protected:
    bool startThread(const char* name, Thread::Priority prio = Thread::Normal);
    void stopThread();
    inline void initThreadOwner(Mutex* mutex)
	{ m_mutex = mutex; }

    YBTSThread* m_thread;
    String m_name;
    Mutex* m_mutex;
};

class YBTSMessage : public GenObject, public YBTSConnIdHolder
{
public:
    inline YBTSMessage(uint8_t pri = 0, uint8_t info = 0, uint16_t cid = 0,
	XmlElement* xml = 0)
        : YBTSConnIdHolder(cid),
        m_primitive(pri), m_info(info), m_xml(xml), m_error(false)
	{}
    ~YBTSMessage()
	{ TelEngine::destruct(m_xml); }
    inline const char* name() const
	{ return lookup(m_primitive,s_priName); }
    inline uint16_t primitive() const
	{ return m_primitive; }
    inline uint8_t info() const
	{ return m_info; }
    inline bool hasConnId() const
	{ return 0 == (m_primitive & 0x80); }
    inline const XmlElement* xml() const
	{ return m_xml; }
    inline XmlElement* takeXml()
	{ XmlElement* x = m_xml; m_xml = 0; return x; }
    inline bool error() const
	{ return m_error; }
    // Parse message. Return 0 on failure
    static YBTSMessage* parse(YBTSSignalling* receiver, uint8_t* data, unsigned int len);
    // Build a message
    static bool build(YBTSSignalling* sender, DataBlock& buf, const YBTSMessage& msg);

    static const TokenDict s_priName[];

    DataBlock m_data;

protected:
    uint8_t m_primitive;
    uint8_t m_info;
    XmlElement* m_xml;
    bool m_error;                        // Encode/decode error flag
};

class YBTSDataSource : public DataSource, public YBTSConnIdHolder
{
public:
    inline YBTSDataSource(const String& format, unsigned int connId, YBTSMedia* media)
        : DataSource(format), YBTSConnIdHolder(connId), m_media(media)
	{}
protected:
    virtual void destroyed();
    YBTSMedia* m_media;
};

class YBTSDataConsumer : public DataConsumer, public YBTSConnIdHolder
{
public:
    inline YBTSDataConsumer(const String& format, unsigned int connId, YBTSMedia* media)
        : DataConsumer(format), YBTSConnIdHolder(connId), m_media(media)
	{}
    virtual unsigned long Consume(const DataBlock& data, unsigned long tStamp,
	unsigned long flags);
protected:
    YBTSMedia* m_media;
};

class YBTSTransport : public GenObject, public YBTSDebug
{
    friend class YBTSLog;
    friend class YBTSCommand;
    friend class YBTSSignalling;
    friend class YBTSMedia;
public:
    inline YBTSTransport()
	: m_maxRead(0)
	{}
    ~YBTSTransport()
	{ resetTransport(); }
    inline const DataBlock& readBuf() const
	{ return m_readBuf; }
    inline HANDLE detachRemote()
	{ return m_remoteSocket.detach(); }
    inline bool canSelect() const
	{ return m_socket.canSelect(); }

protected:
    bool send(const void* buf, unsigned int len);
    inline bool send(const DataBlock& data)
	{ return send(data.data(),data.length()); }
    // Read socket data. Return 0: nothing read, >1: read data, negative: fatal error
    int recv();
    bool initTransport(bool stream, unsigned int buflen, bool reserveNull);
    void resetTransport();
    void alarmError(Socket& sock, const char* oper);

    Socket m_socket;
    Socket m_readSocket;
    Socket m_writeSocket;
    Socket m_remoteSocket;
    DataBlock m_readBuf;
    unsigned int m_maxRead;
};

class YBTSGlobalThread : public Thread, public GenObject
{
public:
    inline YBTSGlobalThread(const char* name, Priority prio = Normal)
	: Thread(name,prio)
	{}
    ~YBTSGlobalThread()
	{ set(this,false); }
    inline void set(YBTSGlobalThread* th, bool add) {
	    if (!th)
		return;
	    Lock lck(s_threadsMutex);
	    if (add)
		s_threads.append(th)->setDelete(false);
	    else
		s_threads.remove(th,false);
	}
    // Cancel all threads, wait to terminate if requested
    // Return true if there are no running threads
    static bool cancelAll(bool hard = false, unsigned int waitMs = 0);

    static ObjList s_threads;
    static Mutex s_threadsMutex;
};

// Holds local area id
class YBTSLAI
{
public:
    inline YBTSLAI(const char* lai = 0)
	: m_lai(lai)
	{}
    YBTSLAI(const XmlElement& xml);
    inline YBTSLAI(const YBTSLAI& other)
	{ *this = other; }
    inline const String& lai() const
	{ return m_lai; }
    inline void set(const char* mcc, const char* mnc, const char* lac) {
	    reset();
	    m_mcc_mnc << mcc << mnc;
	    m_lac = lac;
	    m_lai << m_mcc_mnc << "_" << m_lac;
	}
    inline void reset() {
	    m_mcc_mnc.clear();
	    m_lac.clear();
	    m_lai.clear();
	}
    XmlElement* build() const;
    inline bool operator==(const YBTSLAI& other)
	{ return m_lai == other.m_lai; }
    inline bool operator!=(const YBTSLAI& other)
	{ return m_lai != other.m_lai; }
    inline const YBTSLAI& operator=(const YBTSLAI& other) {
	    m_mcc_mnc = other.m_mcc_mnc;
	    m_lac = other.m_lac;
	    m_lai = other.m_lai;
	    return *this;
	}
protected:
    String m_mcc_mnc;                    // Concatenated MCC + MNC
    String m_lac;                        // LAC
    String m_lai;                        // Concatenated mcc_mnc_lac
};

class YBTSTid : public String
{
public:
    enum Type {
	Unknown = 0,
	Sms,
	Ussd,
    };
    inline YBTSTid(Type t, const String& callRef, bool incoming, uint8_t sapi,
	const char* id = 0)
	: String(id),
	m_type(t), m_callRef(callRef), m_incoming(incoming), m_sapi(sapi),
	m_timeout(0), m_connId(-1), m_paging(false), m_cid(-1)
	{}
    ~YBTSTid();
    inline  const char* typeName() const
	{ return typeName(m_type); }
    // Set connection id, reset conn usage of old connection
    // Increase conn usage on new connection if valid
    void setConnId(int connId);
    // Build SS message (Facility or Release Complete)
    Message* ssMessage(bool facility);
    // Retrieve next CID
    inline int8_t nextCID() {
	    if (m_cid < 127)
		return ++m_cid;
	    return (m_cid = 0);
	}
    static inline ObjList* findObj(ObjList& list, const String& callRef, bool incoming) {
	    for (ObjList* o = list.skipNull(); o; o = o->skipNext()) {
		YBTSTid* i = static_cast<YBTSTid*>(o->get());
		if (i->m_incoming == incoming && i->m_callRef == callRef)
		    return o;
	    }
	    return 0;
	}
    static inline YBTSTid* find(ObjList& list, const String& callRef, bool incoming) {
	    ObjList* o = findObj(list,callRef,incoming);
	    return o ? static_cast<YBTSTid*>(o->get()) : 0;
	}
    static inline YBTSTid* remove(ObjList& list, const String& callRef, bool incoming) {
	    ObjList* o = findObj(list,callRef,incoming);
	    return o ? static_cast<YBTSTid*>(o->remove(false)) : 0;
	}
    static inline YBTSTid* findMO(ObjList& list, const String& callRef)
	{ return find(list,callRef,true); }
    static inline const char* typeName(int t, const char* defVal = "unknown")
	{ return lookup(t,s_typeName,defVal); }
    static const TokenDict s_typeName[];

    Type m_type;
    String m_callRef;                    // TID: Call reference
    bool m_incoming;                     // Transaction direction
    uint8_t m_sapi;                      // SAPI to use
    uint64_t m_timeout;
    int m_connId;                        // Connection id (less then 0: no connection)
    RefPointer<YBTSUE> m_ue;             // Used UE, may not be set
    bool m_paging;                       // Paging the UE
    String m_data;                       // Extra data
    // SS specific data
    String m_peerId;                     // SS session peer id
    String m_startCID;                   // SS start component CID
    int8_t m_cid;                        // Subsequent SS component CID
};

// A logical connection
// UE retrieve/set methods are not thread safe
class YBTSConn : public RefObject, public Mutex, public YBTSConnIdHolder
{
    friend class YBTSSignalling;
    friend class YBTSMM;
    friend class YBTSDriver;
public:
    ~YBTSConn();
    inline YBTSUE* ue()
	{ return m_ue; }
    inline bool waitForTraffic() const
	{ return m_waitForTraffic != 0; }
    // Peek at the pending XML element
    inline const XmlElement* xml() const
	{ return m_xml; }
    // Take out the pending XML element
    inline XmlElement* takeXml()
	{ XmlElement* x = m_xml; m_xml = 0; return x; }
    // Set pending XML. Consume given pointer
    // Return false if another XML element is already pending
    inline bool setXml(XmlElement* xml) {
	    if (!m_xml)
		return ((m_xml = xml) != 0);
	    TelEngine::destruct(xml);
	    return false;
	}
    inline bool hasSS()
	{ return m_ss != 0; }
    inline YBTSTid* takeSS() {
	    YBTSTid* ss = m_ss;
	    m_ss = 0;
	    return ss;
	}
    // Retrieve an SS TID
    inline YBTSTid* findSSTid(const String& callRef, bool incoming) {
	    if (m_ss && m_ss->m_incoming == incoming && m_ss->m_callRef == callRef)
		return m_ss;
	    return 0;
	}
    // Retrieve an SS TID
    inline YBTSTid* findSSTid(const String& ssId) {
	    if (m_ss && ssId == *m_ss)
		return m_ss;
	    return 0;
	}
    // Take (remove) an SS TID
    inline YBTSTid* takeSSTid(const String& callRef, bool incoming) {
	    if (m_ss && m_ss->m_incoming == incoming &&	m_ss->m_callRef == callRef)
		return takeSS();
	    return 0;
	}
    // Take (remove) an SS TID
    inline YBTSTid* takeSSTid(const String& ssId) {
	    if (m_ss && ssId == *m_ss)
		return takeSS();
	    return 0;
	}
    inline YBTSTid* getSSTid(const String& callRef, bool incoming, bool take)
	{ return take ? takeSSTid(callRef,incoming) : findSSTid(callRef,incoming); }
    inline YBTSTid* getSSTid(const String& ssId, bool take)
	{ return take ? takeSSTid(ssId) : findSSTid(ssId); }
    // Start media traffic. Return true is already started, false if requesting
    bool startTraffic(uint8_t mode = 1);
    // Handle media traffic start response
    void startTrafficRsp(bool ok);
    // Start SAPI (values 0..3).
    // Return 0xff if start was sent (wait for establish) or requested SAPI
    uint8_t startSapi(uint8_t sapi);
    // Handle SAPI establish
    void sapiEstablish(uint8_t sapi);
    // Send an L3 connection related message
    bool sendL3(XmlElement* xml, uint8_t info = 0);
protected:
    YBTSConn(YBTSSignalling* owner, uint16_t connId);
    // Set connection UE. Return false if requested to change an existing, different UE
    bool setUE(YBTSUE* ue);

    YBTSSignalling* m_owner;
    XmlElement* m_xml;
    RefPointer<YBTSUE> m_ue;
    ObjList m_sms;                       // Pending sms
    YBTSTid* m_ss;                       // Pending non call related SS transaction
    uint8_t m_traffic;                   // Traffic channel available (mode)
    uint8_t m_waitForTraffic;            // Waiting for traffic to start
    uint8_t m_sapiUp;                    // SAPI status: lower 4 bits SAPI + upper 4 bits if SAPI up on SDCCH
    // The following data is used by YBTSSignalling and protected by conns list mutex
    uint64_t m_timeout;                  // Connection timeout
    unsigned int m_usage;                // Usage counter
    bool m_mtSms;                        // MT SMS was used on this connection
};

class YBTSLog : public GenObject, public DebugEnabler, public Mutex,
    public YBTSThreadOwner
{
public:
    YBTSLog(const char* name);
    inline YBTSTransport& transport()
	{ return m_transport; }
    bool start();
    void stop();
    // Read socket
    virtual void processLoop();
    bool setDebug(Message& msg, const String& target);
protected:
    String m_name;
    YBTSTransport m_transport;
};

class YBTSCommand : public GenObject, public DebugEnabler
{
public:
    YBTSCommand();
    bool send(const String& str);
    bool recv(String& str);
    inline YBTSTransport& transport()
	{ return m_transport; }
    bool start();
    void stop();
    bool setDebug(Message& msg, const String& target);
protected:
    YBTSTransport m_transport;
};

class YBTSSignalling : public GenObject, public DebugEnabler, public Mutex,
    public YBTSThreadOwner
{
public:
    enum State {
	Idle,
	Started,
	WaitHandshake,
	Running,
	Closing
    };
    enum Result {
	Ok = 0,
	Error = 1,
	FatalError = 2
    };
    YBTSSignalling();
    inline int state() const
	{ return m_state; }
    inline bool dumpData() const
	{ return m_printMsg > 0; }
    inline YBTSTransport& transport()
	{ return m_transport; }
    inline GSML3Codec& codec()
	{ return m_codec; }
    inline const YBTSLAI& lai() const
	{ return m_lai; }
    inline void waitHandshake() {
	    Lock lck(this);
	    changeState(WaitHandshake);
	}
    inline bool shouldCheckTimers()
	{ return m_state == Running || m_state == WaitHandshake; }
    int checkTimers(const Time& time = Time());
    // Send a message
    inline bool send(YBTSMessage& msg) {
	    Lock lck(this);
	    return sendInternal(msg);
	}
    // Send an L3 connection related message
    inline bool sendL3Conn(uint16_t connId, XmlElement* xml, uint8_t info = 0) {
	    YBTSMessage m(SigL3Message,info,connId,xml);
	    return send(m);
	}
    // Send L3 RR Status
    bool sendRRMStatus(uint16_t connId, uint8_t code);
    // Send L3 SMS CP Data
    bool sendSmsCPData(YBTSConn* conn, const String& callRef, bool tiFlag,
	uint8_t sapi, const String& rpdu);
    // Send L3 SMS CP Ack/Error
    bool sendSmsCPRsp(YBTSConn* conn, const String& callRef, bool tiFlag,
	uint8_t sapi, const char* cause = 0);
    // Send L3 RP Ack/Error
    bool sendSmsRPRsp(YBTSConn* conn, const String& callRef, bool tiFlag, uint8_t sapi,
	uint8_t rpMsgRef, uint8_t cause);
    // Send SS Facility or Release Complete
    inline bool sendSS(bool facility, YBTSConn* conn, const String& callRef,
	bool tiFlag, uint8_t sapi, const char* cause = 0, const char* facilityIE = 0) {
	    return conn && sendSS(facility,conn->connId(),callRef,tiFlag,sapi,
		cause,facilityIE);
	}
    // Send SS Register
    inline bool sendSSRegister(YBTSConn* conn, const String& callRef,
	uint8_t sapi, const char* facility)
	{ return conn && sendSSRegister(conn->connId(),callRef,sapi,facility); }
    bool start();
    void stop();
    // Drop a connection
    void dropConn(uint16_t connId, bool notifyPeer);
    inline void dropConn(YBTSConn* conn, bool notifyPeer) {
	    if (conn)
		dropConn(conn->connId(),notifyPeer);
	}
    // Drop SS session
    inline void dropSS(YBTSConn* conn, YBTSTid* tid, bool toMs, bool toNetwork,
	const char* reason = 0)
	{ dropSS(conn ? conn->connId() : 0,tid,toMs && conn,toNetwork,reason); }
    // Drop SS session
    void dropAllSS(const char* reason = "net-out-of-order");
    // Increase/decrease connection usage. Update its timeout
    // Return false if connection is not found
    inline bool setConnUsage(uint16_t connId, bool on, bool mtSms = false) {
	    Lock lck(m_connsMutex);
	    YBTSConn* conn = findConnInternal(connId);
	    return conn && setConnUsageInternal(*conn,on,mtSms);
	}
    inline void setConnToutCheck(uint64_t tout) {
	    if (!tout)
		return;
	    Lock lck(m_connsMutex);
	    setConnToutCheckInternal(tout);
	}
    // Add a pending MO sms info to a connection
    // Increase connection usage counter on success
    bool addMOSms(YBTSConn* conn, const String& callRef, uint8_t sapi,
	uint8_t rpCallRef);
    inline bool addMOSms(uint16_t connId, const String& callRef, uint8_t sapi,
	uint8_t rpCallRef) {
	    RefPointer<YBTSConn> conn;
	    findConn(conn,connId,false);
	    return conn && addMOSms(conn,callRef,sapi,rpCallRef);
	}
    // Remove a pending sms info from a connection
    // Decrease connection usage counter on success
    YBTSSmsInfo* removeSms(YBTSConn* conn, const String& callRef, bool incoming);
    // Respond to a MO SMS
    bool moSmsRespond(YBTSConn* conn, const String& callRef, uint8_t cause,
	const String* rpdu = 0);
    inline bool moSmsRespond(uint16_t connId, const String& callRef, uint8_t cause,
	const String* rpdu = 0) {
	    RefPointer<YBTSConn> conn;
	    findConn(conn,connId,false);
	    return conn && moSmsRespond(conn,callRef,cause,rpdu);
	}
    // Handle MO SS execute result
    bool moSSExecuted(YBTSConn* conn, const String& callRef, bool ok,
	const NamedList& params = NamedList::empty());
    inline bool moSSExecuted(uint16_t connId, const String& callRef, bool ok,
	const NamedList& params = NamedList::empty()) {
	    RefPointer<YBTSConn> conn;
	    findConn(conn,connId,false);
	    return conn && moSSExecuted(conn,callRef,ok,params);
	}
    // Read socket. Parse and handle received data
    virtual void processLoop();
    void init(Configuration& cfg);
    inline bool findConn(RefPointer<YBTSConn>& conn, uint16_t connId, bool create) {
	    Lock lck(m_connsMutex);
	    return findConnInternal(conn,connId,create);
	}
    inline bool findConnCreate(RefPointer<YBTSConn>& conn, uint16_t connId,
	bool& created) {
	    Lock lck(m_connsMutex);
	    created = !findConnInternal(conn,connId,false);
	    return created ? findConnInternal(conn,connId,true) : false;
	}
    inline bool findConn(RefPointer<YBTSConn>& conn, const YBTSUE* ue) {
	    Lock lck(m_connsMutex);
	    return findConnInternal(conn,ue);
	}
    // Find a connection containing a given SS transaction
    bool findConnSSTid(RefPointer<YBTSConn>& conn, const String& ssId);

protected:
    inline bool findConnDrop(YBTSMessage& msg, RefPointer<YBTSConn>& conn,
	uint16_t connId) {
	    if (findConn(conn,connId,false))
		return true;
	    Debug(this,DebugNote,"Received %s for non-existent connection %u [%p]",
		msg.name(),connId,this);
	    dropConn(connId,true);
	    return false;
	}
    // Increase/decrease connection usage. Update its timeout
    bool setConnUsageInternal(YBTSConn& conn, bool on, bool mtSms);
    inline void setConnToutCheckInternal(uint64_t tout) {
	    if (tout && (!m_connsTimeout || tout < m_connsTimeout)) {
		m_haveConnTout = true;
		m_connsTimeout = tout;
	    }
	}
    // Send SS Facility or Release Complete
    bool sendSS(bool facility, uint16_t connId, const String& callRef,
	bool tiFlag, uint8_t sapi, const char* cause = 0, const char* facilityIE = 0);
    // Send SS Register
    bool sendSSRegister(uint16_t connId, const String& callRef, uint8_t sapi,
	const char* facility);
    void dropSS(uint16_t connId, YBTSTid* tid, bool toMs, bool toNetwork,
	const char* reason = 0);
    bool sendInternal(YBTSMessage& msg);
    YBTSConn* findConnInternal(uint16_t connId);
    bool findConnInternal(RefPointer<YBTSConn>& conn, uint16_t connId, bool create);
    bool findConnInternal(RefPointer<YBTSConn>& conn, const YBTSUE* ue);
    void changeState(int newStat);
    int handlePDU(YBTSMessage& msg);
    void handleRRM(YBTSMessage& msg);
    int handleHandshake(YBTSMessage& msg);
    void printMsg(YBTSMessage& msg, bool recv);
    void setTimer(uint64_t& dest, const char* name, unsigned int intervalMs,
	uint64_t timeUs = Time::now()) {
	     if (intervalMs) {
		dest = timeUs + (uint64_t)intervalMs * 1000;
		XDebug(this,DebugAll,"%s scheduled in %ums [%p]",name,intervalMs,this);
	    }
	    else if (dest) {
		dest = 0;
		XDebug(this,DebugAll,"%s reset [%p]",name,this);
	    }
	}
    inline void setToutHandshake(uint64_t timeUs = Time::now())
	{ setTimer(m_timeout,"Timeout handshake",m_hkIntervalMs,timeUs); }
    inline void setToutHeartbeat(uint64_t timeUs = Time::now())
	{ setTimer(m_timeout,"Timeout heartbeart",m_hbTimeoutMs,timeUs); }
    inline void setHeartbeatTime(uint64_t timeUs = Time::now())
	{ setTimer(m_hbTime,"Heartbeat interval",m_hbIntervalMs,timeUs); }
    inline void resetHeartbeatTime()
	{ setTimer(m_hbTime,"Heartbeat",0,0); }

    String m_name;
    int m_state;
    int m_printMsg;
    int m_printMsgData;
    YBTSTransport m_transport;
    GSML3Codec m_codec;
    YBTSLAI m_lai;
    Mutex m_connsMutex;
    ObjList m_conns;
    uint64_t m_timeout;
    uint64_t m_hbTime;
    unsigned int m_hkIntervalMs;         // Time (in miliseconds) to wait for handshake
    unsigned int m_hbIntervalMs;         // Heartbeat interval in miliseconds
    unsigned int m_hbTimeoutMs;          // Heartbeat timeout in miliseconds
    // Connection timeout: protected by m_connsMutex
    bool m_haveConnTout;                 // Flag indicating we have connections to timeout
    uint64_t m_connsTimeout;             // Minimum connections timeout
    unsigned int m_connIdleIntervalMs;   // Interval to timeout a connection after becoming idle
    unsigned int m_connIdleMtSmsIntervalMs; // Interval to timeout a connection after becoming idle (MT SMS was used)
};

class YBTSMedia : public GenObject, public DebugEnabler, public Mutex,
    public YBTSThreadOwner
{
    friend class YBTSDataConsumer;
public:
    YBTSMedia();
    inline YBTSTransport& transport()
	{ return m_transport; }
    void setSource(YBTSChan* chan);
    void setConsumer(YBTSChan* chan);
    void addSource(YBTSDataSource* src);
    void removeSource(YBTSDataSource* src);
    void cleanup(bool final);
    bool start();
    void stop();
    // Read socket
    virtual void processLoop();

protected:
    inline void consume(const DataBlock& data, uint16_t connId) {
	    if (!data.length())
		return;
	    uint8_t cid[2] = {(uint8_t)(connId >> 8),(uint8_t)connId};
	    DataBlock tmp(cid,2);
	    tmp += data;
	    m_transport.send(tmp);
	}
    // Find a source object by connection id, return referrenced pointer
    YBTSDataSource* find(unsigned int connId);
    YBTSTransport m_transport;

    String m_name;
    Mutex m_srcMutex;
    ObjList m_sources;
};

class YBTSUE : public RefObject, public Mutex, public YBTSConnIdHolder
{
    friend class YBTSMM;
    friend class YBTSDriver;
public:
    inline const String& imsi() const
	{ return m_imsi; }
    inline String& imsiSafe(String& dest) {
	    Lock lck(this);
	    dest = m_imsi;
	    return dest;
	}
    inline const String& tmsi() const
	{ return m_tmsi; }
    inline const String& imei() const
	{ return m_imei; }
    inline const String& msisdn() const
	{ return m_msisdn; }
    inline const String& paging() const
	{ return m_paging; }
    inline bool registered() const
	{ return m_registered; }
    inline uint32_t expires() const
	{ return m_expires; }
    inline bool removed() const
	{ return m_removed; }
    inline bool imsiDetached() const
	{ return m_imsiDetached; }
    void addCaller(NamedList& list);
    bool startPaging(BtsPagingChanType type);
    void stopPaging();
    void stopPagingNow();

protected:
    inline YBTSUE(const char* imsi, const char* tmsi)
	: Mutex(false,"YBTSUE"),
	  m_registered(false), m_imsiDetached(false), m_removed(false),
	  m_expires(0), m_pageCnt(0),
	  m_imsi(imsi), m_tmsi(tmsi)
	{ }

    bool m_registered;
    bool m_imsiDetached;                 // Unregistered due to IMSI detached
    bool m_removed;                      // Removed from MM list
    uint32_t m_expires;
    uint32_t m_pageCnt;
    String m_imsi;
    String m_tmsi;
    String m_imei;
    String m_msisdn;
    String m_paging;
};

class YBTSLocationUpd : public YBTSGlobalThread, public YBTSConnIdHolder
{
public:
    YBTSLocationUpd(YBTSUE& ue, uint16_t connid);
    ~YBTSLocationUpd()
	{ notify(); }
    inline uint64_t startTime() const
	{ return m_startTime; }
    virtual void cleanup()
	{ notify(); }
protected:
    virtual void run();
    void notify(bool final = true, bool ok = false);

    String m_imsi;                       // UE imsi, empty if already notified termination
    Message m_msg;                       // The message to dispatch
    uint64_t m_startTime;
};

class YBTSSubmit : public YBTSGlobalThread, public YBTSConnIdHolder
{
public:
    YBTSSubmit(YBTSTid::Type t, uint16_t connid, YBTSUE* ue, const char* callRef);
    ~YBTSSubmit()
	{ notify(); }
    inline Message& msg()
	{ return m_msg; }
    virtual void cleanup()
	{ notify(); }
protected:
    virtual void run();
    void notify(bool final = true);

    YBTSTid::Type m_type;
    String m_imsi;                       // UE imsi, empty if already notified termination
    String m_callRef;                    // SMS transaction identifier
    Message m_msg;                       // The message to dispatch
    bool m_ok;
    uint8_t m_cause;
    String m_data;
};

class YBTSSmsInfo : public YBTSTid
{
public:
    inline YBTSSmsInfo(const String& callRef, uint8_t sapi,
	uint8_t rpMsgRef, bool incoming = true)
	: YBTSTid(Sms,callRef,incoming,sapi), m_rpMsgRef(rpMsgRef)
	{}

    uint8_t m_rpMsgRef;                  // RP Message reference
};

class YBTSMtSms : public Mutex, public RefObject
{
    friend class YBTSDriver;
public:
    inline YBTSMtSms(const char* id, const String& rpdu)
	: Mutex(false,"YBTSMtSms"),
	m_id(id), m_active(true), m_sent(false), m_ok(false),
	m_rpdu(rpdu)
	{}
    inline const String& id() const
	{ return m_id; }
    inline bool active() const
	{ return m_active; }
    inline bool sent() const
	{ return m_sent; }
    inline bool ok() const
	{ return m_ok; }
    inline const String& callRef() const
	{ return m_callRef; }
    inline const String& rpdu() const
	{ return m_rpdu; }
    inline const String& response() const
	{ return m_response; }
    inline const String& reason() const
	{ return m_reason; }
    inline void terminate(bool ok, const char* reason = 0, const char* rsp = 0) {
	    Lock lck(this);
	    if (!m_active)
		return;
	    m_active = false;
	    m_ok = ok;
	    m_response = rsp;
	    m_reason = reason;
	}
protected:
    String m_id;
    bool m_active;
    bool m_sent;
    bool m_ok;
    String m_callRef;
    String m_rpdu;
    String m_response;
    String m_reason;
};

class YBTSMtSmsList : public RefObject, public Mutex
{
public:
    inline YBTSMtSmsList(YBTSUE* ue)
	: Mutex(false,"YBTSMtSmsList"), m_tid(0), m_check(false), m_paging(false),
	m_ue(ue)
	{}
    inline YBTSUE* ue()
	{ return m_ue; }
    inline YBTSConn* conn()
	{ return m_conn; }
    inline bool paging() const
	{ return m_paging; }
    inline bool startPaging() {
	    if (!m_paging && m_ue)
		m_paging = m_ue->startPaging(ChanTypeSMS);
	    return m_paging;
	}
    inline void stopPaging() {
	    if (m_paging && m_ue)
		m_ue->stopPaging();
	    m_paging = false;
	}

    ObjList m_sms;                       // MT SMS list
    RefPointer<YBTSConn> m_conn;         // Used connection
    uint8_t m_tid;                       // Current TID
    bool m_check;                        // Flag used to re-check in sms wait loop

protected:
    // Release memory. Decrease connection usage. Stop UE paging
    virtual void destroyed();

    bool m_paging;                       // Paging the UE
    RefPointer<YBTSUE> m_ue;
};

class YBTSMM : public GenObject, public DebugEnabler, public Mutex
{
    friend class YBTSDriver;
public:
    YBTSMM(unsigned int hashLen);
    ~YBTSMM();
    inline XmlElement* buildMM()
	{ return new XmlElement("MM"); }
    inline XmlElement* buildMM(XmlElement*& ch, const char* type) {
	    XmlElement* mm = buildMM();
	    ch = static_cast<XmlElement*>(mm->addChildSafe(new XmlElement(s_message)));
	    ch->setAttribute(s_type,type);
	    return mm;
	}
    inline void saveUEs()
	{ m_saveUEs = true; }
    void handlePDU(YBTSMessage& msg, YBTSConn* conn);
    bool handlePagingResponse(YBTSMessage& m, YBTSConn* conn, XmlElement& rsp);
    void newTMSI(String& tmsi);
    void locUpdTerminated(uint64_t startTime, const String& imsi, uint16_t connId,
	bool ok, const NamedList& params);
protected:
    void handleIdentityResponse(YBTSMessage& m, const XmlElement& xml, YBTSConn* conn);
    void handleLocationUpdate(YBTSMessage& msg, const XmlElement& xml, YBTSConn* conn);
    void handleUpdateComplete(YBTSMessage& m, const XmlElement& xml, YBTSConn* conn);
    void handleIMSIDetach(YBTSMessage& m, const XmlElement& xml, YBTSConn* conn);
    void handleCMServiceRequest(YBTSMessage& msg, const XmlElement& xml, YBTSConn* conn);
    void sendLocationUpdateReject(YBTSMessage& msg, YBTSConn* conn, uint8_t cause);
    void sendCMServiceRsp(YBTSMessage& msg, YBTSConn* conn, uint8_t cause = 0);
    void sendIdentityRequest(YBTSConn* conn, const char* type);
    // Find UE by target identity
    bool findUESafe(RefPointer<YBTSUE>& ue, const String& dest);
    // Find UE by paging identity
    bool findUEPagingSafe(RefPointer<YBTSUE>& ue, const String& paging);
    // Find UE by TMSI
    void findUEByTMSISafe(RefPointer<YBTSUE>& ue, const String& tmsi);
    // Find UE by IMEI
    void findUEByIMEISafe(RefPointer<YBTSUE>& ue, const String& imei);
    // Find UE by MSISDN
    void findUEByMSISDNSafe(RefPointer<YBTSUE>& ue, const String& msisdn);
    // Find UE by IMSI. Create it if not found
    void getUEByIMSISafe(RefPointer<YBTSUE>& ue, const String& imsi, bool create = true);
    inline unsigned int hashList(const String& str)
	{ return str.hash() % m_ueHashLen; }
    // Get IMSI/TMSI from request
    uint8_t getMobileIdentTIMSI(YBTSMessage& m, const XmlElement& request,
	const XmlElement& identXml, const String*& ident, bool& isTMSI);
    // Set UE for a connection
    uint8_t setConnUE(YBTSConn& conn, YBTSUE* ue, const XmlElement& req,
	bool* dropConn = 0);
    void updateExpire(YBTSUE* ue);
    void checkTimers(const Time& time = Time());
    void loadUElist();
    void saveUElist();
    Message* buildUnregister(const String& imsi, YBTSUE* ue = 0);
    void ueRemoved(YBTSUE& ue, const char* reason);

    String m_name;
    Mutex m_ueMutex;
    uint32_t m_tmsiIndex;                // Index used to generate TMSI
    ObjList* m_ueIMSI;                   // List of UEs grouped by IMSI
    ObjList* m_ueTMSI;                   // List of UEs grouped by TMSI
    unsigned int m_ueHashLen;            // Length of UE lists
    bool m_saveUEs;                      // UE list needs saving
};

class YBTSCallDesc : public String, public YBTSConnIdHolder
{
public:
    // Call state
    // ETSI TS 100 940, Section 5.1.2.2
    // Section 10.5.4.6: call state IE
    enum State {
	Null = 0,
	CallInitiated = 1,               // N1: Call initiated
	ConnPending = 2,                 // N0.1: MT call, waiting for MM connection
	CallProceeding = 3,              // N3: MO call proceeding
	CallDelivered = 4,               // N4: Call delivered
	CallPresent = 5,                 // N6: Call present
	CallReceived = 6,                // N7: Call received
	ConnectReq = 8,                  // N8: Connect request
	CallConfirmed = 9,               // N9: MT call confirmed
	Active = 10,                     // N10: Active (answered)
	Disconnect = 12,                 // N12: Disconnect indication
	Release = 19,                    // N19: Release request
	Connect = 28,                    // N28: Connect indication
    };
    // Incoming (MO)
    YBTSCallDesc(YBTSChan* chan, const XmlElement& xml, bool regular, const String* callRef);
    // Outgoing (MT)
    YBTSCallDesc(YBTSChan* chan, const ObjList& xml, const String& callRef);
    ~YBTSCallDesc();
    inline bool incoming() const
	{ return m_incoming; }
    inline const String& callRef() const
	{ return m_callRef; }
    inline bool active() const
	{ return Active == m_state; }
    inline const char* stateName() const
	{ return stateName(m_state); }
    inline const char* reason()
	{ return m_reason.safe("normal"); }
    inline bool setup(ObjList& children) {
	    if (!sendCC(s_ccSetup,children))
		return false;
	    changeState(CallPresent);
	    return true;
	}
    void proceeding();
    void progressing(XmlElement* indicator);
    void alert(XmlElement* indicator);
    void connect(XmlElement* indicator);
    void hangup();
    void sendStatus(const char* cause);
    inline void sendWrongState()
	{ sendStatus("wrong-state-message"); }
    inline void release() {
	    changeState(Release);
	    m_relSent++;
	    sendGSMRel(true,reason(),connId());
	}
    inline void releaseComplete() {
	    changeState(Null);
	    sendGSMRel(false,reason(),connId());
	}
    bool sendCC(const String& tag, XmlElement* c1 = 0, XmlElement* c2 = 0);
    bool sendCC(const String& tag, ObjList& children);
    void changeState(int newState);
    inline void setTimeout(unsigned int intervalMs, const Time& time = Time())
	{ m_timeout = time + (uint64_t)intervalMs * 1000; }
    // Send CC REL or RLC
    static void sendGSMRel(bool rel, const String& callRef, bool tiFlag, const char* reason,
	uint16_t connId);
    inline void sendGSMRel(bool rel, const char* reason, uint16_t connId)
	{ sendGSMRel(rel,callRef(),incoming(),reason,connId); }
    static const char* prefix(bool tiFlag)
	{ return tiFlag ? "o" : "i"; }
    static inline const char* stateName(int stat)
	{ return lookup(stat,s_stateName); }
    static const TokenDict s_stateName[];

    int m_state;
    bool m_incoming;
    bool m_regular;
    uint64_t m_timeout;
    uint8_t m_relSent;
    YBTSChan* m_chan;
    String m_callRef;
    String m_reason;
    String m_called;
};

class YBTSChan : public Channel, public YBTSConnIdHolder
{
public:
    // Incoming
    YBTSChan(YBTSConn* conn);
    // Outgoing
    YBTSChan(YBTSUE* ue, YBTSConn* conn = 0);
    inline YBTSConn* conn() const
	{ return m_conn; }
    inline YBTSUE* ue() const
	{ return m_conn ? m_conn->ue() : (YBTSUE*)m_ue; }
    // Init incoming chan. Return false to destruct the channel
    bool initIncoming(const XmlElement& xml, bool regular, const String* callRef);
    // Init outgoing chan. Return false to destruct the channel
    bool initOutgoing(Message& msg);
    // Handle CC messages
    bool handleCC(const XmlElement& xml, const String* callRef, bool tiFlag);
    // Handle media start/alloc response
    void handleMediaStartRsp(bool ok);
    // Connection released notification
    void connReleased();
    // Check if MT call can be accepted
    bool canAcceptMT();
    // Start a MT call after UE connected
    void startMT(YBTSConn* conn = 0, bool pagingRsp = false);
    // BTS stopping notification
    inline void stop()
	{ hangup("interworking"); }

protected:
    inline YBTSCallDesc* activeCall()
	{ return m_activeCall; }
    inline YBTSCallDesc* firstCall() {
	    ObjList* o = m_calls.skipNull();
	    return o ? static_cast<YBTSCallDesc*>(o->get()) : 0;
	}
    YBTSCallDesc* handleSetup(const XmlElement& xml, bool regular, const String* callRef);
    void hangup(const char* reason = 0, bool final = false);
    inline void setReason(const char* reason, Mutex* mtx = 0) {
	    if (!reason)
		return;
	    Lock lck(mtx);
	    m_reason = reason;
	}
    inline void setTout(uint64_t tout) {
	    if (tout && (!m_tout || m_tout > tout)) {
		m_tout = tout;
		m_haveTout = true;
	    }
	}
    void allocTraffic();
    void stopPaging();
    inline Message* takeRoute() {
	    Lock lck(driver());
	    Message* m = m_route;
	    m_route = 0;
	    return m;
	}
    inline bool route() {
	    Message* m = takeRoute();
	    if (!m)
		return true;
	    m->userData(0);
	    return startRouter(m);
	}
    // Release route message.
    // Deref if we have a pending message and not final and not connected
    bool releaseRoute(bool final = false);
    virtual void disconnected(bool final, const char *reason);
    virtual bool callRouted(Message& msg);
    virtual void callAccept(Message& msg);
    virtual void callRejected(const char* error, const char* reason, const Message* msg);
    virtual bool msgProgress(Message& msg);
    virtual bool msgRinging(Message& msg);
    virtual bool msgAnswered(Message& msg);
    virtual void checkTimers(Message& msg, const Time& tmr);
    virtual bool msgDrop(Message& msg, const char* reason);
#if 0
    virtual bool msgTone(Message& msg, const char* tone);
    virtual bool msgText(Message& msg, const char* text);
    virtual bool msgUpdate(Message& msg);
#endif
    virtual void destroyed();

    Mutex m_mutex;
    RefPointer<YBTSConn> m_conn;
    RefPointer<YBTSUE> m_ue;
    ObjList m_calls;
    String m_reason;
    bool m_waitForTraffic;
    uint8_t m_cref;
    char m_dtmf;
    bool m_mpty;
    bool m_hungup;
    bool m_paging;
    bool m_haveTout;
    uint64_t m_tout;
    YBTSCallDesc* m_activeCall;
    ObjList* m_pending;
    Message* m_route;
};

class YBTSDriver : public Driver
{
public:
    enum Relay {
	Stop = Private,
	Start = Private << 1,
    };
    enum State {
	Idle = 0,
	Starting,                        // The link with peer is starting
	WaitHandshake,                   // The peer was started, waiting for handshake
	Running,                         // Peer hadshake done
	RadioUp
    };
    enum USSDMapOper {
	Pssr = 0,
	Ussr,
	Ussn,
	Pssd,
	USSDMapOperUnknown
    };
    YBTSDriver();
    ~YBTSDriver();
    inline int state() const
	{ return m_state; }
    inline const char* stateName() const
	{ return lookup(m_state,s_stateName); }
    inline bool isPeerCheckState() const
	{ return m_state >= WaitHandshake && m_peerPid; }
    inline void setPeerAlive()
	{ m_peerAlive = true; }
    inline YBTSMedia* media()
	{ return m_media; }
    inline YBTSSignalling* signalling()
	{ return m_signalling; }
    inline YBTSMM* mm()
	{ return m_mm; }
    inline XmlElement* buildCC()
	{ return new XmlElement("CC"); }
    XmlElement* buildCC(XmlElement*& ch, const char* type, const char* callRef, bool tiFlag = false);
    inline void setSmsId(String& s) {
	    s << prefix() << "sms/";
	    Lock lck(this);
	    s << ++m_smsIndex;
	}
    inline const String& ssIdPrefix() const
	{ return m_ssIdPrefix; }
    inline unsigned int ssIndex() {
	    Lock lck(this);
	    return ++m_ssIndex;
	}
    inline Message* message(const char* msg) {
	    Message* m = new Message(msg);
	    m->addParam("module",name());
	    return m;
	}
    inline bool sameModule(NamedList& msg)
	{ return msg[YSTRING("module")] == name(); }
    // Export an xml to a list parameter
    // Consume the xml object
    void exportXml(NamedList& list, XmlElement*& xml, const char* param = "xml");
    // Handle call control messages
    void handleCC(YBTSMessage& m, YBTSConn* conn);
    // Handle SMS PDUs
    void handleSmsPDU(YBTSMessage& m, YBTSConn* conn);
    // Handle SS PDUs
    void handleSSPDU(YBTSMessage& m, YBTSConn* conn);
    // Check if a MT service is pending for new connection and start it
    void checkMtService(YBTSUE* ue, YBTSConn* conn, bool pagingRsp = false);
    // Handle media start/alloc response
    void handleMediaStartRsp(YBTSConn* conn, bool ok);
    // Add a pending (wait termination) call
    void addTerminatedCall(YBTSCallDesc* call);
    // Check terminated calls timeout
    void checkTerminatedCalls(const Time& time = Time());
    // Clear terminated calls for a given connection
    void removeTerminatedCall(uint16_t connId);
    // Handshake done notification. Return false if state is invalid
    bool handshakeDone();
    // Conn release notification
    void connReleased(uint16_t connId);
    // Handle USSD update/finalize messages
    bool handleUssd(Message& msg, bool update);
    // Handle USSD execute messages
    bool handleUssdExecute(Message& msg, String& dest);
    bool getUssdFacility(NamedList& list, String& buf, String& reason, YBTSTid* ss = 0,
	bool update = true);
    // Start an MT USSD. Consume ss on success, might consume it on error
    const char* startMtSs(YBTSConn* conn, YBTSTid*& ss);
    // Radio ready notification. Return false if state is invalid
    bool radioReady();
    void restart(unsigned int restartMs = 1, unsigned int stopIntervalMs = 0);
    void stopNoRestart();
    inline bool findChan(uint16_t connId, RefPointer<YBTSChan>& chan) {
	    Lock lck(this);
	    chan = findChanConnId(connId);
	    return chan != 0;
	}
    inline bool findChan(const YBTSUE* ue, RefPointer<YBTSChan>& chan) {
	    Lock lck(this);
	    chan = findChanUE(ue);
	    return chan != 0;
    }
    // Safely check pending MT SMS
    inline void checkMtSms(YBTSUE* ue) {
	    RefPointer<YBTSMtSmsList> list;
	    if (findMtSmsList(list,ue) && !checkMtSms(*list))
		removeMtSms(list);
	}
    // Check for pending MT SS
    void checkMtSs(YBTSConn* conn);
    // Drop all pending SS
    void dropAllSS(const char* reason = "net-out-of-order");
    // Enqueue an SS related message. Decode facility
    // Don't enqueue facility messages if decode fails
    bool enqueueSS(Message* m, const String* facilityIE, bool facility,
	const char* error = 0);
    // Decode MAP content
    XmlElement* mapDecode(const String& buf, String* error = 0, bool decodeUssd = true);
    // Encode MAP content
    bool mapEncode(XmlElement*& xml, String& buf, String* error = 0,
	bool encodeUssd = true);
    inline bool mapEncodeComp(XmlElement*& xml, String& buf, String* error = 0,
	bool encodeUssd = true) {
	    if (!xml)
		return false;
	    XmlElement* m = new XmlElement("m");
	    m->addChildSafe(xml);
	    xml = 0;
	    return mapEncode(m,buf,error,encodeUssd);
	}
    static inline int ussdOper(const String& name)
	{ return str2index(name,s_ussdOper); }
    static inline const String& ussdOperName(int oper)
	{ return index2str(oper,s_ussdOper); }
    static inline int ussdMapOper(const String& name)
	{ return str2index(name,s_ussdMapOper); }
    static inline const String& ussdMapOperName(int oper)
	{ return index2str(oper,s_ussdMapOper); }
    static const TokenDict s_stateName[];
    static const String s_ussdOper[];
    static const String s_ussdMapOper[];

protected:
    void handleSmsCPData(YBTSMessage& m, YBTSConn* conn,
	const String& callRef, bool tiFlag, const XmlElement& cpData);
    void handleSmsCPRsp(YBTSMessage& m, YBTSConn* conn,
	const String& callRef, bool tiFlag, const XmlElement& rsp, bool ok);
    // Check for MT SMS in list. Return false if the list is empty
    bool checkMtSms(YBTSMtSmsList& list);
    // Handle pending MP SMS final response
    // Return false if not found
    bool handleMtSmsRsp(YBTSUE* ue, bool ok, const String& callRef,
	const char* reason = 0, const char* rpdu = 0, uint8_t respondSapi = 255);
    YBTSMtSmsList* findMtSmsList(YBTSUE* ue, bool create = false);
    inline bool findMtSmsList(RefPointer<YBTSMtSmsList>& list, YBTSUE* ue,
	bool create = false) {
	    if (!ue)
		return false;
            Lock lck(m_mtSmsMutex);
	    list = findMtSmsList(ue,true);
	    return list != 0;
	}
    YBTSMtSmsList* findMtSmsList(uint16_t connId);
    inline bool findMtSmsList(RefPointer<YBTSMtSmsList>& list, uint16_t connId) {
            Lock lck(m_mtSmsMutex);
	    list = findMtSmsList(connId);
	    return list != 0;
	}
    // Remove a pending sms. Remove its queue if empty
    bool removeMtSms(YBTSMtSmsList* list, YBTSMtSms* sms = 0);
    // Handle SS Facility or Release Complete
    void handleSS(YBTSMessage& m, YBTSConn* conn, const String& callRef,
	bool tiFlag, XmlElement& xml, bool facility);
    // Handle SS register
    void handleSSRegister(YBTSMessage& m, YBTSConn* conn, const String& callRef,
	bool tiFlag, XmlElement& xml);
    bool submitUssd(YBTSConn* conn, const String& callRef, const String& ssId,
	XmlElement*& facilityXml, XmlElement* comp);
    void checkMtSsTout(const Time& time = Time());
    void start();
    inline void startIdle() {
	    Lock lck(m_stateMutex);
	    if (m_stopped || !m_engineStart || m_state != Idle || Engine::exiting())
		return;
	    lck.drop();
	    start();
	}
    void stop();
    bool startPeer();
    void stopPeer();
    bool handleMsgExecute(Message& msg, const String& dest);
    bool handleEngineStop(Message& msg);
    YBTSChan* findChanConnId(uint16_t connId);
    YBTSChan* findChanUE(const YBTSUE* ue);
    void changeState(int newStat);
    void setRestart(int resFlag, bool on = true, unsigned int interval = 0);
    void checkStop(const Time& time);
    void checkRestart(const Time& time);
    inline void setStop(unsigned int stopIntervalMs) {
	    m_stop = true;
	    if (m_stopTime)
		return;
	    m_stopTime = Time::now() + (uint64_t)stopIntervalMs * 1000;
	    Debug(this,DebugAll,"Scheduled stop in %ums",stopIntervalMs);
	}
    // Utility called when handling start/stop/restart commands
    inline void cmdStartStop(bool start) {
	    Lock lck(m_stateMutex);
	    m_stopped = !start;
	    m_restartIndex = 0;
	}
    void ybtsStatus(String& line, String& retVal);
    void btsStatus(Message& msg);
    virtual void initialize();
    virtual bool msgExecute(Message& msg, String& dest);
    virtual bool received(Message& msg, int id);
    virtual bool commandExecute(String& retVal, const String& line);
    virtual bool commandComplete(Message& msg, const String& partLine, const String& partWord);
    virtual bool setDebug(Message& msg, const String& target);

    int m_state;
    Mutex m_stateMutex;
    pid_t m_peerPid;                     // Peer PID
    bool m_peerAlive;
    uint64_t m_peerCheckTime;
    unsigned int m_peerCheckIntervalMs;
    bool m_error;                        // Error flag, ignore restart
    bool m_stopped;                      // Stopped by command, don't restart 
    bool m_stop;                         // Stop flag
    uint64_t m_stopTime;                 // Stop time
    bool m_restart;                      // Restart flag
    uint64_t m_restartTime;              // Restart time
    unsigned int m_restartIndex;         // Current restart index
    YBTSLog* m_logTrans;                 // Log transceiver
    YBTSLog* m_logBts;                   // Log OpenBTS
    YBTSCommand* m_command;              // Command interface
    YBTSMedia* m_media;                  // Media
    YBTSSignalling* m_signalling;        // Signalling
    YBTSMM* m_mm;                        // Mobility management
    ObjList m_terminatedCalls;           // Terminated calls list
    bool m_haveCalls;                    // Empty terminated calls list flag
    bool m_engineStart;
    unsigned int m_engineStop;
    ObjList* m_helpCache;
    unsigned int m_smsIndex;             // Index used to generate SMS id
    unsigned int m_ssIndex;              // Index used to generate SS id
    String m_ssIdPrefix;                 // Prefix for SS IDs
    Mutex m_mtSmsMutex;                  // Protects MT SMS list
    ObjList m_mtSms;                     // List of MT SMS
    Mutex m_mtSsMutex;                   // Protects MT SS list
    ObjList m_mtSs;                      // List of pending MT SS
    bool m_mtSsNotEmpty;                 // List of MT SS is not empty
    int m_exportXml;                     // Export xml as string(-1), obj(1) or both(0)
};

class YBTSMsgHandler : public MessageHandler
{
public:
    enum Type {
	UssdExecute = -1,
	UssdUpdate = -2,
	UssdFinalize = -3,
    };
    YBTSMsgHandler(int type, const char* name, int prio);
    virtual bool received(Message& msg);
    static void install();
    static void uninstall();
protected:
    static const TokenDict s_name[];
    static ObjList s_handlers;
    int m_type;
};

INIT_PLUGIN(YBTSDriver);
static Mutex s_globalMutex(false,"YBTSGlobal");
static uint64_t s_startTime = 0;
static uint64_t s_idleTime = Time::now();
static YBTSLAI s_lai;
static String s_configFile;              // Configuration file path
static String s_format = "gsm";          // Format to use
static String s_peerCmd;                 // Peer program command path
static String s_peerArg;                 // Peer program argument
static String s_peerDir;                 // Peer program working directory
static String s_ueFile;                  // File to save UE information
static bool s_askIMEI = true;            // Ask the IMEI identity
static unsigned int s_mtSmsTimeout = YBTS_MT_SMS_TIMEOUT_DEF; // MT SMS timeout interval
static unsigned int s_ussdTimeout = YBTS_USSD_TIMEOUT_DEF;    // USSD session timeout interval
static unsigned int s_bufLenLog = 16384; // Read buffer length for log interface
static unsigned int s_bufLenSign = 1024; // Read buffer length for signalling interface
static unsigned int s_bufLenMedia = 1024;// Read buffer length for media interface
static unsigned int s_restartMs = YBTS_RESTART_DEF; // Time (in miliseconds) to wait for restart
static unsigned int s_restartMax = YBTS_RESTART_COUNT_DEF; // Restart counter
// Call Control Timers (in milliseconds)
// ETSI TS 100 940 Section 11.3
static unsigned int s_t305 = 30000;      // DISC sent, no inband tone available
                                         // Stop when recv REL/DISC, send REL on expire
static unsigned int s_t308 = 5000;       // REL sent (operator specific, no default in spec)
                                         // First expire: re-send REL, second expire: release call
static unsigned int s_t313 = 5000;       // Send Connect, expect Connect Ack, clear call on expire

static uint32_t s_tmsiExpire = 864000;   // TMSI expiration, default 10 days
static bool s_tmsiSave = false;          // Save TMSIs to file

static const String s_statusCmd = "status";
static const String s_startCmd = "start";
static const String s_stopCmd = "stop";
static const String s_restartCmd = "restart";

ObjList YBTSGlobalThread::s_threads;
Mutex YBTSGlobalThread::s_threadsMutex(false,"YBTSGlobalThread");

#define YBTS_MAKENAME(x) {#x, x}
#define YBTS_XML_GETCHILD_PTR_CONTINUE(x,tag,ptr) \
    if (x->getTag() == tag) { \
	ptr = x; \
	continue; \
    }
#define YBTS_XML_GETCHILDTEXT_CHOICE_RETURN(x,tag,ptr) \
    if (x->getTag() == tag) { \
	ptr = &x->getText(); \
	return; \
    }
#define YBTS_XML_GETCHILDTEXT_CHOICE_CONTINUE(x,tag,ptr) \
    if (x->getTag() == tag) { \
	ptr = &x->getText(); \
	continue; \
    }

const TokenDict YBTSMessage::s_priName[] =
{
#define MAKE_SIG(x) {#x, Sig##x}
    MAKE_SIG(L3Message),
    MAKE_SIG(ConnLost),
    MAKE_SIG(ConnRelease),
    MAKE_SIG(StartMedia),
    MAKE_SIG(StopMedia),
    MAKE_SIG(AllocMedia),
    MAKE_SIG(MediaError),
    MAKE_SIG(MediaStarted),
    MAKE_SIG(EstablishSAPI),
    MAKE_SIG(Handshake),
    MAKE_SIG(RadioReady),
    MAKE_SIG(StartPaging),
    MAKE_SIG(StopPaging),
    MAKE_SIG(Heartbeat),
#undef MAKE_SIG
    {0,0}
};

const TokenDict YBTSTid::s_typeName[] = {
    YBTS_MAKENAME(Sms),
    YBTS_MAKENAME(Ussd),
    {0,0}
};

const TokenDict YBTSCallDesc::s_stateName[] =
{
    YBTS_MAKENAME(Null),
    YBTS_MAKENAME(CallInitiated),
    YBTS_MAKENAME(ConnPending),
    YBTS_MAKENAME(CallProceeding),
    YBTS_MAKENAME(CallDelivered),
    YBTS_MAKENAME(CallPresent),
    YBTS_MAKENAME(CallReceived),
    YBTS_MAKENAME(ConnectReq),
    YBTS_MAKENAME(CallConfirmed),
    YBTS_MAKENAME(Active),
    YBTS_MAKENAME(Disconnect),
    YBTS_MAKENAME(Release),
    YBTS_MAKENAME(Connect),
    {0,0}
};

const TokenDict YBTSDriver::s_stateName[] =
{
    YBTS_MAKENAME(Idle),
    YBTS_MAKENAME(Starting),
    YBTS_MAKENAME(WaitHandshake),
    YBTS_MAKENAME(Running),
    YBTS_MAKENAME(RadioUp),
    {0,0}
};

const String YBTSDriver::s_ussdOper[] = {"pssr", "ussr", "ussn", "pssd", ""};

const String YBTSDriver::s_ussdMapOper[] = {
    "processUnstructuredSS-Request",
    "unstructuredSS-Request",
    "unstructuredSS-Notify",
    "processUnstructuredSS-Data",
    ""
};

const TokenDict YBTSMsgHandler::s_name[] = {
    {"ussd.execute",  UssdExecute},
    {"ussd.update",   UssdUpdate},
    {"ussd.finalize", UssdFinalize},
    {0,0}
};
ObjList YBTSMsgHandler::s_handlers;

static const TokenDict s_numType[] = {
    {"unknown",             0},
    {"international",       1},
    {"national",            2},
    {"network-specific",    3},
    {"dedicated-access",    4},
    {"reserved",            5},
    {"abbreviated",         6},
    {"extension-reserved",  7},
    {0,0}
};

static const TokenDict s_numPlan[] = {
    {"unknown",            0},
    {"isdn",               1},
    {"data",               3},
    {"telex",              4},
    {"national",           8},
    {"private",            9},
    {"CTS-reserved",       11},
    {"extension-reserved", 15},
    {0,0}
};

static const char* s_bcd = "0123456789*#ABC";

// RP message type
// ETSI TS 124.011 Section 7.3 and 8.2:
// 1 byte (3 bits): message type
//   0: RP-DATA - MS to network
//   1: RP-DATA - network to MS
//   2: RP-ACK - MS to network
//   3: RP-ACK - network to MS
//   4: RP-ERROR - MS to network
//   5: RP-ERROR - network to MS
//   6: RP-SMMA - MS to network
enum RPMsgType
{
    RPDataFromMs = 0,
    RPDataFromNetwork = 1,
    RPAckFromMs = 2,
    RPAckFromNetwork = 3,
    RPErrorFromMs = 4,
    RPErrorFromNetwork = 3,
    RPSMMAFromMs = 6,
};

// Skip length and value from buffer
// Length is 1 byte value length
static inline void skipLV(uint8_t*& b, unsigned int& len)
{
    if (!len)
	return;
    unsigned int skip = *b + 1;
    if (skip > len)
	skip = len;
    b += skip;
    len -= skip;
}

// Decode BCD number
static void decodeBCDNumber(uint8_t* b, unsigned int len,
    String& bcd, String* plan, String* type, bool ignoreLast = false)
{
    if (!len)
	return;
    if (plan)
	*plan = lookup(*b & 0x0f,s_numPlan);
    if (type)
	*type = lookup((*b & 0x70) >> 4,s_numType);
    if (len == 1)
	return;
    len--;
    b++;
    char* s = new char[len * 2 + 1];
    unsigned int l = 0;
    for (; len; b++, len--) {
	uint8_t idx = (*b & 0x0f);
	if (idx < 15)
	    s[l++] = s_bcd[idx];
	if (ignoreLast && len == 1)
	    break;
	idx = *b >> 4;
	if (idx < 15)
	    s[l++] = s_bcd[idx];
    }
    if (l)
	bcd.assign(s,l);
    delete[] s;
}

// Retrieve the TBCD char index
// Return 15 (0x0f) if invalid
static inline uint8_t bcdIndex(char c)
{
    for (uint8_t i = 0; i < 15; i++)
	if (c == s_bcd[i])
	    return i;
    return 15;
}

// Encode BCD number
// len is bcd number length
static unsigned int encodeBCDNumber(uint8_t* b, unsigned int len,
    const char* bcd, const char* plan, const char* type)
{
    uint8_t p = (uint8_t)lookup(plan,s_numPlan);
    uint8_t t = (uint8_t)lookup(type,s_numType);
    *b++ = 0x80 | (p & 0x0f) | ((t & 0x03) << 4);
    if (!(bcd && len))
	return 1;
    unsigned int n = 1;
    for (unsigned int i = 0; (i < len) && *bcd; i++) {
	uint8_t first = bcdIndex(*bcd++);
	if (first >= 15)
	    return n;
	uint8_t second = 15;
	if (*bcd) {
	    second = bcdIndex(*bcd++);
	    if (second >= 15)
		return n;
	}
	*b++ = (second << 4) | first;
	n++;
    }
    return n;
}

// Retrieve RP message type and reference from buffer
// Retrieve source/destination addr for RP-DATA
// Return 0 on success
// -1: Unable to retrieve message type and reference
// -2: Unable to retrieve party addr
// ETSI TS 124.011 Section 7.3 and 8.2:
// 1 byte (3 bits): message type
// 1 byte message reference
// Other IEs ...
// RP-DATA IEs:
//   RP-Originator Address
//   RP-Destination Address
//   RP-User Data
static int decodeRP(uint8_t*& b, unsigned int& len, uint8_t& rpMsgType,
    uint8_t& rpMsgRef, String* bcd = 0, String* plan = 0, String* type = 0)
{
    if (len < 2)
	return -1;
    rpMsgType = *b++ & 0x07;
    rpMsgRef = *b++;
    len -= 2;
    if (!bcd || (rpMsgType != RPDataFromMs && rpMsgType != RPDataFromNetwork))
	return 0;
    // MS -> network: skip originator, retrieve destination
    // network -> MS: retrieve originator, skip destination
    if (rpMsgType == RPDataFromMs) {
	skipLV(b,len);
	if (!len)
	    return -2;
    }
    unsigned int destLen = *b++;
    if (destLen) {
	if (destLen <= len)
	    decodeBCDNumber(b,destLen,*bcd,plan,type);
	else
	    destLen = len;
	b += destLen;
	len -= destLen;
    }
    if (rpMsgType == RPDataFromNetwork)
	skipLV(b,len);
    return 0;
}

// Retrieve RP message type and reference from buffer
// Retrieve source/destination addr for RP-DATA
// Return 0 on success
// -1: Unable to retrieve message type and reference
// -2: Unable to retrieve party addr
// 1: Invalid string
static int decodeRP(const String& str, uint8_t& rpMsgType,
    uint8_t& rpMsgRef, String* bcd = 0, String* plan = 0, String* type = 0,
    String* smsParty = 0, String* smsPartyPlan = 0, String* smsPartyType = 0,
    String* smsText = 0, String* smsTextEnc = 0)
{
    DataBlock d;
    if (!d.unHexify(str))
	return 1;
    uint8_t* b = (uint8_t*)d.data();
    unsigned int len = d.length();
    int res = decodeRP(b,len,rpMsgType,rpMsgRef,bcd,plan,type);
    if (res || !len)
	return res;
    if (rpMsgType != RPDataFromMs && rpMsgType != RPDataFromNetwork)
	return 0;
    if (!(smsParty || smsText))
	return 0;
    // RP-User-Data length
    unsigned int rpLen = *b++;
    len--;
    if (rpLen > len)
	return 0;
    // See TS 123.040 Section 9
    uint8_t tp = *b++;
    len--;
    uint8_t tpMTI = tp & 0x03;
    // SMS SUBMIT, Section 9.2.2.2
    if (tpMTI == 1) {
	if (len < 2)
	    return 0;
	// Skip message ref
	b++;
	len--;
	// Handle destination address
	if (len < 2)
	    return 0;
	uint8_t addrLen = *b++;
	len--;
	unsigned int l = 1 + (addrLen + 1) / 2;
	if (smsParty) {
	    bool odd = (addrLen & 0x01) != 0;
	    if (l <= len)
		decodeBCDNumber(b,l,*smsParty,smsPartyPlan,smsPartyType,odd);
	    else
		l = len;
	}
	else if (l > len)
	    l = len;
	b += l;
	len -= l;
	if (!len)
	    return 0;
	// Done if text is not required
	if (!smsText)
	    return 0;
	// Skip Protocol Identifier
	b++;
	len--;
	if (!len)
	    return 0;
	// Data Coding scheme, handle only GSM 7bit
	if (*b)
	    return 0;
	b++;
	len--;
	if (!len)
	    return 0;
	// Skip validity period if present
	uint8_t vp = ((tp >> 3) & 0x03);
	if (vp) {
	    l = (vp == 2) ? 1 : 7;
	    if (l > len)
		l = len;
	    b += l;
	    len -= l;
	    if (!len)
		return 0;
	}
	// User data length
	l = *b++;
	len--;
	if (!len || l > len)
	    return 0;
	// User data
	// Skip 1 byte header if present
	if ((tp & 0x40) != 0) {
	    Debug(&__plugin,DebugNote,"Can't decode SMS text with header");
	    return 0;
	}
	GSML3Codec::decodeGSM7Bit(b,l,*smsText);
	if (smsText->length() > l)
	    *smsText = smsText->substr(0,l);
	// Remove
	if (smsTextEnc)
	    *smsTextEnc = "gsm7bit";
    }
    return 0;
}

static void moveList(ObjList& dest, ObjList& src)
{
    ObjList* a = &dest;
    for (ObjList* o = src.skipNull(); o; o = o->skipNull())
	a = a->append(o->remove(false));
    src.clear();
}

// Append an xml element from list parameter
static inline bool addXmlFromParam(ObjList& dest, const NamedList& list,
    const char* tag, const String& param)
{
    const String& p = list[param];
    if (p)
	dest.append(new XmlElement(tag,p));
    return !p.null();
}

// Safely retrieve a global string
static inline void getGlobalStr(String& buf, const String& value)
{
    Lock lck(s_globalMutex);
    buf = value;
}

// Safely retrieve global uint64_t
// Safely retrieve a global string
static inline void getGlobalUInt64(uint64_t& buf, const uint64_t& value)
{
    Lock lck(s_globalMutex);
    buf = value;
}

static inline bool isValidStartTime(uint64_t val)
{
    uint64_t cmp = 0;
    getGlobalUInt64(cmp,s_startTime);
    return cmp && cmp <= val;
}

// Add a socket error to a buffer
static inline String& addLastError(String& buf, int error, const char* prefix = ": ")
{
    String tmp;
    Thread::errorString(tmp,error);
    buf << prefix << error << " " << tmp;
    return buf;
}

// Calculate a number of thread idle intervals from a period
// Return a non 0 value
static inline unsigned int threadIdleIntervals(unsigned int intervalMs)
{
    intervalMs = (unsigned int)(intervalMs / Thread::idleMsec());
    return intervalMs ? intervalMs : 1;
}

static inline const NamedList& safeSect(Configuration& cfg, const String& name)
{
    const NamedList* sect = cfg.getSection(name);
    return sect ? *sect : NamedList::empty();
}

// Check if a string is composed of digits in interval 0..9
static inline bool isDigits09(const String& str)
{
    if (!str)
	return false;
    for (const char* s = str.c_str(); *s; s++)
	if (*s < '0' || *s > '9')
	    return false;
    return true;
}

// Retrieve XML children
static inline void findXmlChildren(const XmlElement& xml,
    XmlElement*& ptr1, const String& tag1,
    XmlElement*& ptr2, const String& tag2)
{
    for (const ObjList* o = xml.getChildren().skipNull(); o; o = o->skipNext()) {
	XmlElement* x = static_cast<XmlChild*>(o->get())->xmlElement();
	if (!x)
	    continue;
	YBTS_XML_GETCHILD_PTR_CONTINUE(x,tag1,ptr1);
	YBTS_XML_GETCHILD_PTR_CONTINUE(x,tag2,ptr2);
    }
}

// Retrieve the text of an xml child
// Use this function for choice XML
static inline void getXmlChildTextChoice(const XmlElement& xml,
    const String*& ptr1, const String& tag1,
    const String*& ptr2, const String& tag2)
{
    for (const ObjList* o = xml.getChildren().skipNull(); o; o = o->skipNext()) {
	XmlElement* x = static_cast<XmlChild*>(o->get())->xmlElement();
	if (!x)
	    continue;
	YBTS_XML_GETCHILDTEXT_CHOICE_RETURN(x,tag1,ptr1);
	YBTS_XML_GETCHILDTEXT_CHOICE_RETURN(x,tag2,ptr2);
    }
}

// Retrieve the text of a requested xml children
static inline void getXmlChildTextAll(const XmlElement& xml,
    const String*& ptr1, const String& tag1,
    const String*& ptr2, const String& tag2)
{
    for (const ObjList* o = xml.getChildren().skipNull(); o; o = o->skipNext()) {
	XmlElement* x = static_cast<XmlChild*>(o->get())->xmlElement();
	if (!x)
	    continue;
	YBTS_XML_GETCHILDTEXT_CHOICE_CONTINUE(x,tag1,ptr1);
	YBTS_XML_GETCHILDTEXT_CHOICE_CONTINUE(x,tag2,ptr2);
    }
}

// Build an xml element with a child
static inline XmlElement* buildXmlWithChild(const String& tag, const String& childTag,
    const char* childText = 0)
{
    XmlElement* xml = new XmlElement(tag);
    xml->addChildSafe(new XmlElement(childTag,childText));
    return xml;
}

// Retrieve IMSI or TMSI from xml
static inline const String* getIdentTIMSI(const XmlElement& xml, bool& isTMSI, bool& found)
{
    const String* imsi = 0;
    const String* tmsi = 0;
    getXmlChildTextChoice(xml,imsi,s_imsi,tmsi,s_tmsi);
    found = (imsi || tmsi);
    isTMSI = (tmsi != 0);
    if (!found)
	return 0;
    if (tmsi)
	return *tmsi ? tmsi : 0;
    return *imsi ? imsi : 0;
}

static inline void getCCCause(String& dest, const XmlElement& xml)
{
    dest = xml.childText(s_cause);
}

static inline XmlElement* buildCCCause(const char* cause, const char* location = "LPN",
    const char* coding = "GSM-PLMN")
{
    if (TelEngine::null(cause))
	cause = "normal";
    XmlElement* xml = new XmlElement(s_cause,cause);
    if (coding)
	xml->setAttribute(s_causeCoding,coding);
    if (location)
	xml->setAttribute(s_causeLocation,location);
    return xml;
}

static inline XmlElement* buildCCCause(const String& cause)
{
    const char* location = "LPN";
    if (cause == YSTRING("busy") || cause == YSTRING("noresponse") || cause == YSTRING("noanswer"))
	location = "U";
    return buildCCCause(cause,location,"GSM-PLMN");
}

static inline void getCCCallState(String& dest, const XmlElement& xml)
{
    dest = xml.childText(s_ccCallState);
}

static inline XmlElement* buildCCCallState(int stat)
{
    return new XmlElement(s_ccCallState,String(stat));
}

// Build Progress Indicator IE
static inline XmlElement* buildProgressIndicator(const Message& msg,
    bool earlyMedia = true)
{
    const String& s = msg[YSTRING("progress.indicator")];
    if (s)
	return new XmlElement(s_ccProgressInd,s);
    if (earlyMedia && msg.getBoolValue("earlymedia"))
	return new XmlElement(s_ccProgressInd,"inband");
    return 0;
}

// Retrieve TID (transaction identifier data)
// Return true if a valid transaction identifier was found
static inline bool getTID(const XmlElement& xml, const String*& callRef, bool& tiFlag)
{
    const XmlElement* tid = xml.findFirstChild(&s_ccCallRef);
    if (tid) {
	callRef = &tid->getText();
	if (TelEngine::null(callRef))
	    callRef = 0;
	tiFlag = tid->attributes().getBoolValue(s_ccTIFlag);
	return callRef != 0;
    }
    return false;
}

// Build TID (transaction identifier data)
static inline XmlElement* buildTID(const char* callRef, bool tiFlag)
{
    XmlElement* tid = new XmlElement(s_ccCallRef,callRef);
    tid->setAttribute(s_ccTIFlag,String::boolText(tiFlag));
    return tid;
}


//
// YBTSThread
//
YBTSThread::YBTSThread(YBTSThreadOwner* owner, const char* name, Thread::Priority prio)
    : Thread(name,prio),
    m_owner(owner)
{
}

void YBTSThread::cleanup()
{
    notifyTerminate();
}

void YBTSThread::run()
{
    if (!m_owner)
	return;
    m_owner->processLoop();
    notifyTerminate();
}

void YBTSThread::notifyTerminate()
{
    if (!m_owner)
	return;
    m_owner->threadTerminated(this);
    m_owner = 0;
}


//
// YBTSThreadOwner
//
bool YBTSThreadOwner::startThread(const char* name, Thread::Priority prio)
{
    m_thread = new YBTSThread(this,name,prio);
    if (m_thread->startup()) {
	Debug(m_enabler,DebugAll,"Started worker thread [%p]",m_ptr);
	return true;
    }
    m_thread = 0;
    Alarm(m_enabler,"system",DebugWarn,"Failed to start worker thread [%p]",m_ptr);
    return false;
}

void YBTSThreadOwner::stopThread()
{
    Lock lck(m_mutex);
    if (!m_thread)
	return;
    DDebug(m_enabler,DebugAll,"Stopping worker thread [%p]",m_ptr);
    m_thread->cancel();
    while (m_thread) {
	lck.drop();
	Thread::idle();
	lck.acquire(m_mutex);
    }
}

void YBTSThreadOwner::threadTerminated(YBTSThread* th)
{
    Lock lck(m_mutex);
    if (!(m_thread && th && m_thread == th))
	return;
    m_thread = 0;
    Debug(m_enabler,DebugAll,"Worker thread terminated [%p]",m_ptr);
}


//
// YBTSMessage
//
// Utility used in YBTSMessage::parse()
static inline void decodeMsg(GSML3Codec& codec, uint8_t* data, unsigned int len,
    XmlElement*& xml, String& reason)
{
    unsigned int e = codec.decode(data,len,xml);
    if (e)
	reason << "Codec error " << e << " (" << lookup(e,GSML3Codec::s_errorsDict,"unknown") << ")";
}

// Parse message. Return 0 on failure
YBTSMessage* YBTSMessage::parse(YBTSSignalling* recv, uint8_t* data, unsigned int len)
{
    if (!len)
	return 0;
    if (len < 2) {
	Debug(recv,DebugNote,"Received short message length %u [%p]",len,recv);
	return 0;
    }
    YBTSMessage* m = new YBTSMessage;
    if (recv->dumpData())
	m->m_data.assign(data,len);
    m->m_primitive = *data++;
    m->m_info = *data++;
    len -= 2;
    if (m->hasConnId()) {
	if (len < 2) {
	    Debug(recv,DebugNote,
		"Received message %u (%s) with missing connection id [%p]",
		m->primitive(),m->name(),recv);
	    TelEngine::destruct(m);
	    return 0;
	}
	m->setConnId(ntohs(*(uint16_t*)data));
	data += 2;
	len -= 2;
    }
    String reason;
    switch (m->primitive()) {
	case SigL3Message:
#ifdef DEBUG
	    {
		String tmp;
		tmp.hexify(data,len,' ');
		Debug(recv,DebugAll,"Recv L3 message: %s",tmp.c_str());
	    }
#endif
	    decodeMsg(recv->codec(),data,len,m->m_xml,reason);
	    break;
	case SigEstablishSAPI:
	case SigHandshake:
	case SigRadioReady:
	case SigHeartbeat:
	case SigConnLost:
	case SigMediaError:
	case SigMediaStarted:
	    break;
	default:
	    reason = "No decoder";
    }
    if (reason) {
	Debug(recv,DebugNote,"Failed to parse %u (%s): %s [%p]",
	    m->primitive(),m->name(),reason.c_str(),recv);
	m->m_error = true;
    }
    return m;
}

// Utility used in YBTSMessage::parse()
static inline bool encodeMsg(GSML3Codec& codec, const YBTSMessage& msg, DataBlock& buf,
    String& reason)
{
    if (msg.xml()) {
	unsigned int e = codec.encode(msg.xml(),buf);
	if (!e)
	    return true;
	reason << "Codec error " << e << " (" << lookup(e,GSML3Codec::s_errorsDict,"unknown") << ")";
    }
    else
	reason = "Empty XML";
    return false;
}

// Build a message
bool YBTSMessage::build(YBTSSignalling* sender, DataBlock& buf, const YBTSMessage& msg)
{
    uint8_t b[4] = {msg.primitive(),msg.info()};
    if (msg.hasConnId()) {
	uint8_t* p = b;
	*(uint16_t*)(p + 2) = htons(msg.connId());
	buf.append(b,4);
    }
    else
	buf.append(b,2);
    String reason;
    switch (msg.primitive()) {
	case SigL3Message:
	    if (encodeMsg(sender->codec(),msg,buf,reason)) {
#ifdef DEBUG
		void* data = buf.data(4);
		if (data) {
		    String tmp;
		    tmp.hexify(data,buf.length() - 4,' ');
		    Debug(sender,DebugAll,"Send L3 message: %s",tmp.c_str());
		}
#endif
		return true;
	    }
	    break;
	case SigStartPaging:
	case SigStopPaging:
	    if (!msg.xml()) {
		reason = "Missing XML";
		break;
	    }
	    buf.append(msg.xml()->getText());
	    return true;
	case SigHeartbeat:
	case SigHandshake:
	case SigConnRelease:
	case SigStartMedia:
	case SigStopMedia:
	case SigAllocMedia:
	case SigEstablishSAPI:
	    return true;
	default:
	    reason = "No encoder";
    }
    Debug(sender,DebugNote,"Failed to build %s (%u): %s [%p]",
	msg.name(),msg.primitive(),reason.c_str(),sender);
    return false;
}


//
// YBTSDataSource
//
void YBTSDataSource::destroyed()
{
    if (m_media)
	m_media->removeSource(this);
    DataSource::destroyed();
}


//
// YBTSDataConsumer
//
unsigned long YBTSDataConsumer::Consume(const DataBlock& data, unsigned long tStamp,
    unsigned long flags)
{
    if (m_media)
	m_media->consume(data,connId());
    return invalidStamp();
}


//
// YBTSTransport
//
bool YBTSTransport::send(const void* buf, unsigned int len)
{
    if (!len)
	return true;
    int wr = m_writeSocket.send(buf,len);
    if (wr == (int)len)
	return true;
    if (wr >= 0)
	Debug(m_enabler,DebugNote,"Sent %d/%u [%p]",wr,len,m_ptr);
    else if (!m_writeSocket.canRetry())
	alarmError(m_writeSocket,"send");
    return false;
}

// Read socket data. Return 0: nothing read, >1: read data, negative: fatal error
int YBTSTransport::recv()
{
    if (!m_readSocket.valid())
	return 0;
    if (canSelect()) {
	bool ok = false;
	if (!m_readSocket.select(&ok,0,0,Thread::idleUsec())) {
	    if (m_readSocket.canRetry())
		return 0;
	    alarmError(m_readSocket,"select");
	    return -1;
	}
	if (!ok)
	    return 0;
    }
    uint8_t* buf = (uint8_t*)m_readBuf.data();
    int rd = m_readSocket.recv(buf,m_maxRead);
    if (rd >= 0) {
	if (rd) {
	    __plugin.setPeerAlive();
	    if (m_maxRead < m_readBuf.length())
		buf[rd] = 0;
#ifdef XDEBUG
	    String tmp;
	    tmp.hexify(buf,rd,' ');
	    Debug(m_enabler,DebugAll,"Read %d bytes: %s [%p]",rd,tmp.c_str(),m_ptr);
#endif
	}
	return rd;
    }
    if (m_readSocket.canRetry())
	return 0;
    alarmError(m_readSocket,"read");
    return -1;
}

bool YBTSTransport::initTransport(bool stream, unsigned int buflen, bool reserveNull)
{
    resetTransport();
    String error;
#ifdef PF_UNIX
    SOCKET pair[2];
    if (::socketpair(PF_UNIX,stream ? SOCK_STREAM : SOCK_DGRAM,0,pair) == 0) {
	m_socket.attach(pair[0]);
	m_remoteSocket.attach(pair[1]);
	if (m_socket.setBlocking(false)) {
	    m_readSocket.attach(m_socket.handle());
	    m_writeSocket.attach(m_socket.handle());
	    m_readBuf.assign(0,reserveNull ? buflen + 1 : buflen);
	    m_maxRead = reserveNull ? m_readBuf.length() - 1 : m_readBuf.length();
	    return true;
	}
	error << "Failed to set non blocking mode";
        addLastError(error,m_socket.error());
    }
    else {
	error << "Failed to create pair";
        addLastError(error,errno);
    }
#else
    error = "UNIX sockets not supported";
#endif
    Alarm(m_enabler,"socket",DebugWarn,"%s [%p]",error.c_str(),m_ptr);
    return false;
}

void YBTSTransport::resetTransport()
{
    m_socket.terminate();
    m_readSocket.detach();
    m_writeSocket.detach();
    m_remoteSocket.terminate();
}

void YBTSTransport::alarmError(Socket& sock, const char* oper)
{
    String tmp;
    addLastError(tmp,sock.error());
    Alarm(m_enabler,"socket",DebugWarn,"Socket %s error%s [%p]",
	oper,tmp.c_str(),m_ptr);
}


//
// YBTSLAI
//
// Cancel all threads, wait to terminate if requested
// Return true if there are no running threads
bool YBTSGlobalThread::cancelAll(bool hard, unsigned int waitMs)
{
    Lock lck(s_threadsMutex);
    ObjList* o = s_threads.skipNull();
    if (!o)
	return true;
    Debug(&__plugin,hard ? DebugWarn : DebugAll,
	"Cancelling%s %u global threads",hard ? " hard" : "",o->count());
    for (; o; o = o->skipNext())
	static_cast<YBTSGlobalThread*>(o->get())->cancel(hard);
    if (hard) {
	s_threads.clear();
	return true;
    }
    if (!waitMs)
	return false;
    unsigned int n = threadIdleIntervals(waitMs);
    while (n--) {
	lck.drop();
	Thread::idle();
	lck.acquire(s_threadsMutex);
	if (!s_threads.skipNull())
	    return true;
    }
    return false;
}


//
// YBTSLAI
//
YBTSLAI::YBTSLAI(const XmlElement& xml)
{
    const String* mcc_mnc = &String::empty();
    const String* lac = &String::empty();
    getXmlChildTextAll(xml,mcc_mnc,s_PLMNidentity,lac,s_LAC);
    m_mcc_mnc = *mcc_mnc;
    m_lac = *lac;
    m_lai << m_mcc_mnc << "_" << m_lac;
}

XmlElement* YBTSLAI::build() const
{
    XmlElement* xml = new XmlElement(s_locAreaIdent);
    if (m_mcc_mnc)
	xml->addChildSafe(new XmlElement(s_PLMNidentity,m_mcc_mnc));
    if (m_lac)
	xml->addChildSafe(new XmlElement(s_LAC,m_lac));
    return xml;
}


//
// YBTSConn
//
YBTSConn::YBTSConn(YBTSSignalling* owner, uint16_t connId)
    : Mutex(false,"YBTSConn"),
    YBTSConnIdHolder(connId),
    m_owner(owner), m_xml(0),
    m_ss(0),
    m_traffic(0),
    m_waitForTraffic(0),
    m_sapiUp(1),
    m_timeout(0),
    m_usage(0),
    m_mtSms(false)
{
}

YBTSConn::~YBTSConn()
{
    TelEngine::destruct(m_xml);
    TelEngine::destruct(m_ss);
}

bool YBTSConn::startTraffic(uint8_t mode)
{
    if (mode == m_traffic)
	return true;
    Lock lck(this);
    if (mode == m_traffic)
	return true;
    if (m_waitForTraffic == mode)
	return false;
    m_waitForTraffic = mode;
    lck.drop();
    YBTSMessage m(SigStartMedia,mode,connId());
    __plugin.signalling()->send(m);
    Debug(__plugin.signalling(),DebugAll,
	"Connection %u waiting for traffic channel allocation mode=%u ... [%p]",
	connId(),mode,__plugin.signalling());
    return false;
}

// Handle media traffic start response
void YBTSConn::startTrafficRsp(bool ok)
{
    if (m_waitForTraffic == m_traffic)
	return;
    Lock lck(this);
    if (m_waitForTraffic == m_traffic)
	return;
    Debug(__plugin.signalling(),ok ? DebugAll : DebugNote,
	"Connection %u traffic channel set %s mode=%u [%p]",
	connId(),(ok ? "succeded" : "failed"),m_waitForTraffic,__plugin.signalling());
    if (ok)
	m_traffic = m_waitForTraffic;
    m_waitForTraffic = 0;
}

// Start SAPI (values 0..3)
// Return 0xff if start was sent (wait for establish) or requested SAPI
uint8_t YBTSConn::startSapi(uint8_t sapi)
{
    if (!sapi)
	return 0;
    if (sapi > 3)
	return 255;
    // Do nothing if waiting for traffic to start
    if (waitForTraffic())
	return 255;
    Lock lck(this);
    uint8_t s = (m_sapiUp & 0x0f) >> sapi;
    if (s) {
	uint8_t upper = (m_sapiUp & 0xf0) >> sapi;
	if (upper)
	    sapi |= 0x80;
	return sapi;
    }
    if (m_traffic)
	sapi |= 0x80;
    lck.drop();
    // Send SAPI establish
    YBTSMessage m(SigEstablishSAPI,sapi,connId());
    __plugin.signalling()->send(m);
    return 255;
}

// Handle SAPI establish
void YBTSConn::sapiEstablish(uint8_t sapi)
{
    if (!sapi || sapi == 0x80)
	return;
    uint8_t n = sapi & 0x0f;
    if (n > 3)
	return;
    Lock lck(this);
    n = (1 << n);
    m_sapiUp |= n;
    uint8_t upper = (n << 4);
    if ((sapi & 0x80) != 0)
	m_sapiUp |= upper;
    else
	m_sapiUp &= ~upper;
    Debug(__plugin.signalling(),DebugAll,"Connection %u sapi establish %u state 0x%02x [%p]",
	connId(),sapi,m_sapiUp,__plugin.signalling());
}

// Send an L3 connection related message
bool YBTSConn::sendL3(XmlElement* xml, uint8_t info)
{
    return m_owner->sendL3Conn(connId(),xml,info);
}

// Set connection UE. Return false if requested to change an existing, different UE
bool YBTSConn::setUE(YBTSUE* ue)
{
    if (!ue)
	return true;
    if (!m_ue) {
	m_ue = ue;
	Debug(m_owner,DebugAll,
	    "Connection %u set UE (%p) imsi=%s tmsi=%s [%p]",
	    connId(),ue,ue->imsi().c_str(),ue->tmsi().c_str(),this);
	return true;
    }
    if (m_ue == ue)
	return true;
    Debug(m_owner,DebugMild,
	"Can't replace UE on connection %u: existing=(%p,%s,%s) new=(%p,%s,%s) [%p]",
	connId(),(YBTSUE*)m_ue,m_ue->imsi().c_str(),m_ue->tmsi().c_str(),
	ue,ue->imsi().c_str(),ue->tmsi().c_str(),this);
    return false;
}


//
// YBTSLog
//
YBTSLog::YBTSLog(const char* name)
    : Mutex(false,"YBTSLog"),
      m_name(name)
{
    debugName(m_name);
    debugChain(&__plugin);
    YBTSThreadOwner::initThreadOwner(this);
    YBTSThreadOwner::setDebugPtr(this,this);
    m_transport.setDebugPtr(this,this);
}

bool YBTSLog::start()
{
    stop();
    while (true) {
	Lock lck(this);
	if (!m_transport.initTransport(false,s_bufLenLog,true))
	    break;
	if (!startThread("YBTSLog"))
	    break;
	Debug(this,DebugInfo,"Started [%p]",this);
	return true;
    }
    stop();
    return false;
}

void YBTSLog::stop()
{
    stopThread();
    Lock lck(this);
    if (!m_transport.m_socket.valid())
	return;
    m_transport.resetTransport();
    Debug(this,DebugInfo,"Stopped [%p]",this);
}

// Read socket
void YBTSLog::processLoop()
{
    while (!Thread::check(false)) {
	int rd = m_transport.recv();
	if (rd > 2) {
	    int level = -1;
	    switch (m_transport.readBuf().at(0)) {
		case LOG_EMERG:
		    level = DebugGoOn;
		    break;
		case LOG_ALERT:
		case LOG_CRIT:
		    level = DebugWarn;
		    break;
		case LOG_ERR:
		case LOG_WARNING:
		    level = DebugMild;
		    break;
		case LOG_NOTICE:
		    level = DebugNote;
		    break;
		case LOG_INFO:
		    level = DebugInfo;
		    break;
		case LOG_DEBUG:
		    level = DebugAll;
		    break;
	    }
	    String tmp((const char*)m_transport.readBuf().data(1));
	    // LF -> CR LF
	    for (int i = 0; (i = tmp.find('\n',i + 1)) >= 0; ) {
		if (tmp.at(i - 1) != '\r') {
		    tmp = tmp.substr(0,i) + "\r" + tmp.substr(i);
		    i++;
		}
	    }
	    if (level >= 0)
		Debug(this,level,"%s",tmp.c_str());
	    else
		Output("%s",tmp.c_str());
	    continue;
	}
	if (!rd) {
	    if (!m_transport.canSelect())
		Thread::idle();
	    continue;
	}
	// Socket non retryable error
	__plugin.restart();
	break;
    }
}

bool YBTSLog::setDebug(Message& msg, const String& target)
{
    String str = msg.getValue("line");
    if (str.startSkip("level")) {
	int dbg = debugLevel();
	str >> dbg;
	debugLevel(dbg);
    }
    else if (str == "reset")
	debugChain(&__plugin);
    else {
	bool dbg = debugEnabled();
	str >> dbg;
	debugEnabled(dbg);
    }
    msg.retValue() << "Module " << target
	<< " debug " << (debugEnabled() ? "on" : "off")
	<< " level " << debugLevel() << "\r\n";
    return true;
}


//
// YBTSCommand
//
YBTSCommand::YBTSCommand()
{
    debugName("ybts-command");
    debugChain(&__plugin);
    m_transport.setDebugPtr(this,this);
}

bool YBTSCommand::send(const String& str)
{
    if (m_transport.send(str.c_str(),str.length()))
	return true;
    __plugin.restart();
    return false;
}

bool YBTSCommand::recv(String& str)
{
    uint32_t t = Time::secNow() + 10;
    while (!Thread::check(false)) {
	int rd = m_transport.recv();
	if (rd > 0) {
	    str = (const char*)m_transport.readBuf().data();
	    return true;
	}
	if (!rd) {
	    if (!m_transport.canSelect())
		Thread::idle();
	    if (Time::secNow() < t)
		continue;
	}
	// Socket non retryable error
	__plugin.restart();
	return false;
    }
    return false;
}

bool YBTSCommand::start()
{
    stop();
    if (!m_transport.initTransport(false,s_bufLenLog,true))
	return false;
    Debug(this,DebugInfo,"Started [%p]",this);
    return true;
}

void YBTSCommand::stop()
{
    if (!m_transport.m_socket.valid())
	return;
    m_transport.resetTransport();
    Debug(this,DebugInfo,"Stopped [%p]",this);
}


//
// YBTSSignalling
//
YBTSSignalling::YBTSSignalling()
    : Mutex(false,"YBTSSignalling"),
    m_state(Idle),
    m_printMsg(-1),
    m_printMsgData(1),
    m_codec(0),
    m_connsMutex(false,"YBTSConns"),
    m_timeout(0),
    m_hbTime(0),
    m_hkIntervalMs(YBTS_HK_INTERVAL_DEF),
    m_hbIntervalMs(YBTS_HB_INTERVAL_DEF),
    m_hbTimeoutMs(YBTS_HB_TIMEOUT_DEF),
    m_haveConnTout(false),
    m_connsTimeout(0),
    m_connIdleIntervalMs(2000),
    m_connIdleMtSmsIntervalMs(5000)
{
    m_name = "ybts-signalling";
    debugName(m_name);
    debugChain(&__plugin);
    YBTSThreadOwner::initThreadOwner(this);
    YBTSThreadOwner::setDebugPtr(this,this);
    m_transport.setDebugPtr(this,this);
    m_codec.setCodecDebug(this,this);
}

int YBTSSignalling::checkTimers(const Time& time)
{
    // Check timers
    Lock lck(this);
    if (m_state == Closing || m_state == Idle)
	return Ok;
    if (m_timeout && m_timeout <= time) {
	Alarm(this,"system",DebugWarn,"Timeout while waiting for %s [%p]",
	    (m_state != WaitHandshake  ? "heartbeat" : "handshake"),this);
	changeState(Closing);
	return Error;
    }
    if (m_hbTime && m_hbTime <= time) {
	static uint8_t buf[2] = {SigHeartbeat,0};
	DDebug(this,DebugAll,"Sending heartbeat [%p]",this);
	lck.drop();
	bool ok = m_transport.send(buf,2);
	lck.acquire(this);
	if (ok)
	    setHeartbeatTime(time);
	else if (m_state == Running) {
	    changeState(Closing);
	    return Error;
	}
    }
    lck.drop();
    if (m_haveConnTout) {
	ObjList removeSS;
	ObjList remove;
	m_connsMutex.lock();
	if (m_connsTimeout && m_connsTimeout <= time) {
	    m_connsTimeout = 0;
	    for (ObjList* o = m_conns.skipNull(); o; o = o->skipNext()) {
		YBTSConn* c = static_cast<YBTSConn*>(o->get());
		if (c->m_ss && c->m_ss->m_timeout) {
		    if (Engine::exiting() || c->m_ss->m_timeout <= time) {
			removeSS.append(c->m_ss);
			c->m_ss = 0;
		    }
		    else
			setConnToutCheckInternal(c->m_ss->m_timeout);
		}
		if (!c->m_timeout)
		    continue;
		if (Engine::exiting() || c->m_timeout <= time)
		    remove.append(new String(c->connId()));
		else
		    setConnToutCheckInternal(c->m_timeout);
	    }
	}
	m_haveConnTout = (m_connsTimeout != 0);
	m_connsMutex.unlock();
	for (ObjList* o = removeSS.skipNull(); o; o = o->skipNext()) {
	    YBTSTid* ss = static_cast<YBTSTid*>(o->get());
	    dropSS(ss->m_connId,ss,ss->m_connId >= 0,true,"timeout");
	}
	for (ObjList* o = remove.skipNull(); o; o = o->skipNext()) {
	    String* s = static_cast<String*>(o->get());
	    if (!Engine::exiting())
		Debug(this,DebugAll,"Connection %s idle timeout [%p]",s->c_str(),this);
	    dropConn(s->toInteger(),true);
	}
    }
    return Ok;
}

bool YBTSSignalling::sendRRMStatus(uint16_t connId, uint8_t code)
{
    XmlElement* rrm = new XmlElement("RRM");
    XmlElement* ch =  new XmlElement(s_message);
    ch->setAttribute(s_type,"RRStatus");
    ch->addChildSafe(new XmlElement("RRCause",String(code)));
    return sendL3Conn(connId,rrm);
}

// Send L3 SMS CP Data
bool YBTSSignalling::sendSmsCPData(YBTSConn* conn, const String& callRef, bool tiFlag,
    uint8_t sapi, const String& rpdu)
{
    if (!conn)
	return false;
    XmlElement* sms = new XmlElement("SMS");
    sms->addChildSafe(buildTID(callRef,tiFlag));
    XmlElement* what = new XmlElement(s_message);
    what->setAttribute(s_type,"CP-Data");
    what->addChildSafe(new XmlElement("RPDU",rpdu));
    sms->addChildSafe(what);
    return sendL3Conn(conn->connId(),sms,sapi);
}

// Send L3 SMS CP Ack/Error
bool YBTSSignalling::sendSmsCPRsp(YBTSConn* conn, const String& callRef, bool tiFlag,
   uint8_t sapi, const char* cause)
{
    if (!conn)
	return false;
    XmlElement* sms = new XmlElement("SMS");
    sms->addChildSafe(buildTID(callRef,tiFlag));
    XmlElement* what = new XmlElement(s_message);
    if (!cause)
	what->setAttribute(s_type,"CP-Ack");
    else {
	what->setAttribute(s_type,"CP-Error");
	what->addChildSafe(new XmlElement("CP-Cause",String(cause)));
    }
    sms->addChildSafe(what);
    return sendL3Conn(conn->connId(),sms,sapi);
}

// Send L3 RP Ack/Error
bool YBTSSignalling::sendSmsRPRsp(YBTSConn* conn, const String& callRef, bool tiFlag,
    uint8_t sapi, uint8_t rpMsgRef, uint8_t cause)
{
    // ETSI TS 124 011
    // Section 7.3.3 and 7.3.4: RP Ack/Error: 1 byte message type + 1 byte message reference
    // Error: RP-Cause IE: 1 byte length + 1 byte cause
    // See Section 8.2.2 for message type
    uint8_t buf[4] = {!cause ? RPAckFromNetwork : RPErrorFromNetwork,rpMsgRef};
    String rpdu;
    if (!cause)
	rpdu.hexify(buf,2);
    else {
	buf[2] = 1;
	buf[3] = (cause & 0x7f);
	rpdu.hexify(buf,4);
    }
    return sendSmsCPData(conn,callRef,tiFlag,sapi,rpdu);
}

bool YBTSSignalling::start()
{
    stop();
    while (true) {
	s_globalMutex.lock();
	m_lai = s_lai;
	s_globalMutex.unlock();
	if (!m_lai.lai()) {
	    Debug(this,DebugNote,"Failed to start: invalid LAI [%p]",this);
	    break;
	}
	Lock lck(this);
	if (!m_transport.initTransport(false,s_bufLenSign,true))
	    break;
	if (!startThread("YBTSSignalling"))
	    break;
	changeState(Started);
	Debug(this,DebugInfo,"Started [%p]",this);
	return true;
    }
    stop();
    return false;
}

void YBTSSignalling::stop()
{
    stopThread();
    dropAllSS();
    m_connsMutex.lock();
    ObjList conns;
    moveList(conns,m_conns);
    m_connsMutex.unlock();
    conns.clear();
    Lock lck(this);
    changeState(Idle);
    if (!m_transport.m_socket.valid())
	return;
    m_transport.resetTransport();
    Debug(this,DebugInfo,"Stopped [%p]",this);
}

// Drop a connection
void YBTSSignalling::dropConn(uint16_t connId, bool notifyPeer)
{
    RefPointer<YBTSConn> conn;
    Lock lck(m_connsMutex);
    for (ObjList* o = m_conns.skipNull(); o; o = o->skipNext()) {
	YBTSConn* c = static_cast<YBTSConn*>(o->get());
	if (c->connId() != connId)
	    continue;
	conn = c;
	Debug(this,DebugAll,"Removing connection (%p,%u) [%p]",c,connId,this);
	o->remove();
	break;
    }
    lck.drop();
    if (conn) {
	conn->lock();
	YBTSTid* ss = conn->takeSS();
	conn->unlock();
	if (ss) {
	    dropSS(conn,ss,notifyPeer,true,"net-out-of-order");
	    TelEngine::destruct(ss);
	}
	if (notifyPeer) {
	    YBTSMessage m(SigConnRelease,0,connId);
	    send(m);
	}
	conn = 0;
    }
    __plugin.connReleased(connId);
}

// Drop SS session
void YBTSSignalling::dropAllSS(const char* reason)
{
    ObjList removeSS;
    m_connsMutex.lock();
    for (ObjList* o = m_conns.skipNull(); o; o = o->skipNext()) {
	YBTSConn* c = static_cast<YBTSConn*>(o->get());
	c->lock();
	YBTSTid* ss = c->takeSS();
	c->unlock();
	if (ss)
	    removeSS.append(ss);
    }
    m_connsMutex.unlock();
    for (ObjList* o = removeSS.skipNull(); o; o = o->skipNext()) {
	YBTSTid* ss = static_cast<YBTSTid*>(o->get());
	dropSS(ss->m_connId,ss,ss->m_connId >= 0,true,reason);
    }
}

// Add a pending MO sms info to a connection
// Increase connection usage counter on success
bool YBTSSignalling::addMOSms(YBTSConn* conn, const String& callRef, uint8_t sapi,
    uint8_t rpCallRef)
{
    if (!conn)
	return false;
    Lock lck(conn);
    if (YBTSSmsInfo::findMO(conn->m_sms,callRef))
	return true;
    YBTSSmsInfo* i = new YBTSSmsInfo(callRef,sapi,rpCallRef);
    conn->m_sms.append(i);
    XDebug(this,DebugAll,"Added MO SMS (%p) tid=%s to conn %u [%p]",
	i,i->m_callRef.c_str(),conn->connId(),this);
    lck.drop();
    setConnUsage(conn->connId(),true);
    return true;
}

// Remove a pending sms info from a connection
// Decrease connection usage counter on success
YBTSSmsInfo* YBTSSignalling::removeSms(YBTSConn* conn, const String& callRef,
    bool incoming)
{
    if (!conn)
	return 0;
    Lock lck(conn);
    YBTSTid* tid = YBTSTid::remove(conn->m_sms,callRef,incoming);
    YBTSSmsInfo* i = static_cast<YBTSSmsInfo*>(tid);
    if (!i)
	return 0;
    XDebug(this,DebugAll,"Removed %s SMS (%p) tid=%s from conn %u [%p]",
	(incoming ? "MO" : "MT"),i,i->m_callRef.c_str(),conn->connId(),this);
    lck.drop();
    setConnUsage(conn->connId(),false);
    return i;
}

// Respond to a MO SMS
bool YBTSSignalling::moSmsRespond(YBTSConn* conn, const String& callRef, uint8_t cause,
    const String* rpdu)
{
    if (!conn)
	return false;
    Lock lck(conn);
    YBTSSmsInfo* i = static_cast<YBTSSmsInfo*>(YBTSTid::findMO(conn->m_sms,callRef));
    if (!i) {
	DDebug(this,DebugInfo,"No MO SMS to respond tid=%s conn=%u [%p]",
	    callRef.c_str(),conn->connId(),this);
	return false;
    }
    uint8_t sapi = i->m_sapi;
    uint8_t rpMsgRef = i->m_rpMsgRef;
    lck.drop();
    if (TelEngine::null(rpdu))
	sendSmsRPRsp(conn,callRef,true,sapi,rpMsgRef,cause);
    else
	sendSmsCPData(conn,callRef,true,sapi,*rpdu);
    return true;
}

bool YBTSSignalling::moSSExecuted(YBTSConn* conn, const String& callRef, bool ok,
    const NamedList& params)
{
    if (!conn)
	return false;
    Lock lck(conn);
    YBTSTid* tid = conn->getSSTid(callRef,true,!ok);
    if (!tid)
	return false;
    if (ok)
	tid->m_peerId = params[YSTRING("peerid")];
    else {
	lck.drop();
	dropSS(conn,tid,true,false,params.getValue(s_error));
	TelEngine::destruct(tid);
    }
    return true;
}

// Read socket. Parse and handle received data
void YBTSSignalling::processLoop()
{
    while (!Thread::check(false)) {
	int rd = m_transport.recv();
	if (rd > 0) {
	    uint8_t* buf = (uint8_t*)m_transport.readBuf().data();
	    YBTSMessage* m = YBTSMessage::parse(this,buf,rd);
	    if (m) {
		lock();
		setToutHeartbeat();
		unlock();
		int res = Ok;
		if (m->primitive() != SigHeartbeat)
		    res = handlePDU(*m);
		else
		    DDebug(this,DebugAll,"Received heartbeat [%p]",this);
		TelEngine::destruct(m);
		if (res) {
		    if (res == FatalError)
			__plugin.stopNoRestart();
		    else
			__plugin.restart();
		    break;
		}
	    }
	    continue;
	}
	if (rd) {
	    // Socket non retryable error
	    __plugin.restart();
	    break;
	}
	if (!m_transport.canSelect())
	    Thread::idle();
    }
}

static inline int getPrintData(const NamedList& list, const String& param, int defVal)
{
    const String& s = list[param];
    if (!s)
	return defVal;
    if (!s.isBoolean())
	return 1;
    return s.toBoolean() ? -1 : 0;
}

void YBTSSignalling::init(Configuration& cfg)
{
    const NamedList& ybts = safeSect(cfg,YSTRING("ybts"));
    m_hkIntervalMs = ybts.getIntValue(YSTRING("handshake_start"),
	YBTS_HK_INTERVAL_DEF,YBTS_HK_INTERVAL_MIN,YBTS_HK_INTERVAL_MAX);
    m_hbIntervalMs = ybts.getIntValue(YSTRING("heartbeat_ping"),
	YBTS_HB_INTERVAL_DEF,YBTS_HB_INTERVAL_MIN,YBTS_HB_INTERVAL_MAX);
    m_hbTimeoutMs = ybts.getIntValue(YSTRING("heartbeat_timeout"),
	YBTS_HB_TIMEOUT_DEF,m_hbIntervalMs + 3000,YBTS_HB_TIMEOUT_MAX);
    m_printMsg = getPrintData(ybts,YSTRING("print_msg"),-1);
    m_printMsgData = getPrintData(ybts,YSTRING("print_msg_data"),1);
#ifdef DEBUG
    String s;
    s << "\r\nheartbeat_ping=" << m_hbIntervalMs;
    s << "\r\nheartbeat_timeout=" << m_hbTimeoutMs;
    s << "\r\nhandshake_start=" << m_hkIntervalMs;
    s << "\r\nprint_msg=" <<
	(m_printMsg > 0 ? "dump" : String::boolText(m_printMsg < 0));
    s << "\r\nprint_msg_data=" <<
	(m_printMsgData > 0 ? "verbose" : String::boolText(m_printMsgData < 0));
    Debug(this,DebugAll,"Initialized [%p]\r\n-----%s\r\n-----",this,s.c_str());
#endif
}

// Find a connection containing a given SS transaction
bool YBTSSignalling::findConnSSTid(RefPointer<YBTSConn>& conn, const String& ssId)
{
    Lock lck(m_connsMutex);
    for (ObjList* o = m_conns.skipNull(); o; o = o->skipNext()) {
	YBTSConn* c = static_cast<YBTSConn*>(o->get());
	Lock lckConn(c);
	if (c->findSSTid(ssId)) {
	    conn = c;
	    return conn != 0;
	}
    }
    return false;
}

// Increase/decrease connection usage. Update its timeout
bool YBTSSignalling::setConnUsageInternal(YBTSConn& conn, bool on, bool mtSms)
{
    if (on)
	conn.m_usage++;
    else if (conn.m_usage)
	conn.m_usage--;
    if (mtSms && on)
	conn.m_mtSms = true;
    if (conn.m_usage)
	conn.m_timeout = 0;
    else if (!conn.m_timeout) {
	if (!conn.m_mtSms)
	    conn.m_timeout = Time::now() + (uint64_t)m_connIdleIntervalMs * 1000;
	else
	    conn.m_timeout = Time::now() + (uint64_t)m_connIdleMtSmsIntervalMs * 1000;
	setConnToutCheckInternal(conn.m_timeout);
    }
    return true;
}

bool YBTSSignalling::sendSS(bool facility, uint16_t connId, const String& callRef,
	bool tiFlag, uint8_t sapi, const char* cause, const char* facilityIE)
{
    XmlElement* ss = new XmlElement("SS");
    ss->addChildSafe(buildTID(callRef,tiFlag));
    XmlElement* what = new XmlElement(s_message);
    what->setAttribute(s_type,facility ? s_facility : s_rlc);
    if (facilityIE)
	what->addChildSafe(new XmlElement(s_facility,facilityIE));
    if (cause)
	what->addChildSafe(new XmlElement(s_cause,cause));
    ss->addChildSafe(what);
    return sendL3Conn(connId,ss,sapi);
}

// Send SS Register
bool YBTSSignalling::sendSSRegister(uint16_t connId, const String& callRef, uint8_t sapi,
    const char* facility)
{
    XmlElement* ss = new XmlElement("SS");
    ss->addChildSafe(buildTID(callRef,false));
    XmlElement* what = new XmlElement(s_message);
    what->setAttribute(s_type,"Register");
    what->addChildSafe(new XmlElement(s_facility,facility));
    ss->addChildSafe(what);
    return sendL3Conn(connId,ss,sapi);
}

// Drop SS session
void YBTSSignalling::dropSS(uint16_t connId, YBTSTid* ss, bool toMs, bool toNetwork,
    const char* reason)
{
    if (!(ss && (toMs || toNetwork)))
	return;
    Debug(this,DebugInfo,"Dropping SS session '%s' type=%s conn=%d reason=%s [%p]",
	ss->c_str(),ss->typeName(),ss->m_connId,reason,this);
    if (toMs)
	sendSS(false,connId,ss->m_callRef,ss->m_incoming,ss->m_sapi,reason);
    if (toNetwork)
	__plugin.enqueueSS(ss->ssMessage(false),0,false,reason);
}

// Send a message
bool YBTSSignalling::sendInternal(YBTSMessage& msg)
{
    YBTSMessage::build(this,msg.m_data,msg);
    if (m_printMsg && debugAt(DebugInfo))
	printMsg(msg,false);
    if (!m_transport.send(msg.m_data))
	return false;
    setHeartbeatTime();
    return true;
}

YBTSConn* YBTSSignalling::findConnInternal(uint16_t connId)
{
    for (ObjList* o = m_conns.skipNull(); o; o = o->skipNext()) {
	YBTSConn* c = static_cast<YBTSConn*>(o->get());
	if (c->connId() == connId)
	    return c;
    }
    return 0;
}

bool YBTSSignalling::findConnInternal(RefPointer<YBTSConn>& conn, uint16_t connId, bool create)
{
    conn = 0;
    YBTSConn* c = findConnInternal(connId);
    if (c) {
	conn = c;
	return true;
    }
    if (create) {
	conn = new YBTSConn(this,connId);
	m_conns.append(conn);
	Debug(this,DebugAll,"Added connection (%p,%u) [%p]",
	    (YBTSConn*)conn,conn->connId(),this);
    }
    return conn != 0;
}

bool YBTSSignalling::findConnInternal(RefPointer<YBTSConn>& conn, const YBTSUE* ue)
{
    conn = 0;
    for (ObjList* o = m_conns.skipNull(); o; o = o->skipNext()) {
	YBTSConn* c = static_cast<YBTSConn*>(o->get());
	if (c->ue() == ue) {
	    conn = c;
	    return true;
	}
    }
    return false;
}

void YBTSSignalling::changeState(int newStat)
{
    if (m_state == newStat)
	return;
    DDebug(this,DebugInfo,"State changed %d -> %d [%p]",m_state,newStat,this);
    m_state = newStat;
    switch (m_state) {
	case Idle:
	case Closing:
	    setTimer(m_timeout,"Timeout",0,0);
	    resetHeartbeatTime();
	    break;
	case WaitHandshake:
	    setToutHandshake();
	    resetHeartbeatTime();
	    break;
	case Running:
	case Started:
	    break;
    }
}

int YBTSSignalling::handlePDU(YBTSMessage& msg)
{
    if (m_printMsg && debugAt(DebugInfo))
	printMsg(msg,true);
    switch (msg.primitive()) {
	case SigL3Message:
	    if (m_state != Running) {
		Debug(this,DebugNote,"Ignoring %s in non running state [%p]",
		    msg.name(),this);
		return Ok;
	    }
	    if (msg.xml()) {
		const String& proto = msg.xml()->getTag();
		RefPointer<YBTSConn> conn;
		if (proto == YSTRING("MM")) {
		    findConn(conn,msg.connId(),true);
		    __plugin.mm()->handlePDU(msg,conn);
		}
		else if (proto == YSTRING("CC")) {
		    if (findConnDrop(msg,conn,msg.connId()))
			__plugin.handleCC(msg,conn);
		}
		else if (proto == YSTRING("SMS")) {
		    if (findConnDrop(msg,conn,msg.connId()))
			__plugin.handleSmsPDU(msg,conn);
		}
		else if (proto == YSTRING("SS")) {
		    if (findConnDrop(msg,conn,msg.connId()))
			__plugin.handleSSPDU(msg,conn);
		}
		else if (proto == YSTRING("RRM"))
		    handleRRM(msg);
		else
		    Debug(this,DebugNote,"Unknown '%s' protocol in %s [%p]",
			proto.c_str(),msg.name(),this);
	    }
	    else if (msg.error()) {
		// TODO: close message connection ?
	    }
	    else {
		// TODO: put some error
	    }
	    return Ok;
	case SigMediaStarted:
	case SigMediaError:
	    if (msg.connId()) {
		bool ok = (msg.primitive() == SigMediaStarted);
		RefPointer<YBTSConn> conn;
		findConn(conn,msg.connId(),false);
		if (conn)
		    conn->startTrafficRsp(ok);
		__plugin.handleMediaStartRsp(conn,ok);
	    }
	    return Ok;
	case SigEstablishSAPI:
	    if (msg.connId()) {
		RefPointer<YBTSConn> conn;
		findConn(conn,msg.connId(),false);
		if (conn) {
		    conn->sapiEstablish(msg.info());
		    __plugin.checkMtService(conn->ue(),conn);
		}
	    }
	    return Ok;
	case SigHandshake:
	    return handleHandshake(msg);
	case SigRadioReady:
	    __plugin.radioReady();
	    return Ok;
	case SigConnLost:
	    dropConn(msg.connId(),false);
	    return Ok;
    }
    Debug(this,DebugNote,"Unhandled message %u (%s) [%p]",msg.primitive(),msg.name(),this);
    return Ok;
}

void YBTSSignalling::handleRRM(YBTSMessage& m)
{
    XmlElement* ch = m.xml() ? m.xml()->findFirstChild(&s_message) : 0;
    if (!ch) {
	Debug(this,DebugNote,"Empty xml in %s [%p]",m.name(),this);
	return;
    }
    const String* t = ch->getAttribute(s_type);
    if (!t)
	Debug(this,DebugWarn,"Missing 'type' in %s [%p]",m.name(),this);
    else if (*t == YSTRING("PagingResponse")) {
	RefPointer<YBTSConn> conn;
	bool newConn = false;
	findConnCreate(conn,m.connId(),newConn);
	if (!__plugin.mm()->handlePagingResponse(m,conn,*ch) && newConn)
	    dropConn(m.connId(),true);
    }
    else
	Debug(this,DebugMild,"Unhandled '%s' in %s [%p]",t->c_str(),m.name(),this);
}

int YBTSSignalling::handleHandshake(YBTSMessage& msg)
{
    Lock lck(this);
    if (state() != WaitHandshake) {
	Debug(this,DebugNote,"Unexpected handshake [%p]",this);
	return Ok;
    }
    lck.drop();
    if (!msg.info()) {
	if (__plugin.handshakeDone()) {
	    lck.acquire(this);
	    if (state() != WaitHandshake)
		return Ok;
	    changeState(Running);
	    YBTSMessage m(SigHandshake);
	    if (sendInternal(m))
		return Ok;
	}
    }
    else {
	Alarm(this,"system",DebugWarn,"Unknown version %u in handshake [%p]",
	    msg.info(),this);
	return FatalError;
    }
    return Error;
}

void YBTSSignalling::printMsg(YBTSMessage& msg, bool recv)
{
    String s;
    s << "\r\n-----";
    if (dumpData() && msg.m_data.length()) {
	String tmp;
	tmp.hexify((void*)msg.m_data.data(),msg.m_data.length(),' ');
	s << "\r\nBUFFER: " << tmp;
    }
    const char* tmp = msg.name();
    s << "\r\nPrimitive: ";
    if (tmp)
	s << tmp;
    else
	s << msg.primitive();
    s << "\r\nInfo: " << msg.info();
    if (msg.hasConnId())
	s << "\r\nConnection: " << msg.connId();
    if (m_printMsgData) {
	String data;
	if (msg.xml()) {
	    String indent;
	    String origindent;
	    if (m_printMsgData > 0) {
		indent << "\r\n";
		origindent << "  ";
	    }
	    msg.xml()->toString(data,false,indent,origindent);
	}
	s.append(data,"\r\n");
    }
    s << "\r\n-----";
    Debug(this,DebugInfo,"%s [%p]%s",recv ? "Received" : "Sending",this,s.safe());
}


//
// YBTSMedia
//
YBTSMedia::YBTSMedia()
    : Mutex(false,"YBTSMedia"),
    m_srcMutex(false,"YBTSMediaSourceList")
{
    m_name = "ybts-media";
    debugName(m_name);
    debugChain(&__plugin);
    YBTSThreadOwner::initThreadOwner(this);
    YBTSThreadOwner::setDebugPtr(this,this);
    m_transport.setDebugPtr(this,this);
}

void YBTSMedia::setSource(YBTSChan* chan)
{
    if (!chan)
	return;
    String format;
    getGlobalStr(format,s_format);
    YBTSDataSource* src = new YBTSDataSource(format,chan->connId(),this);
    addSource(src);
    chan->setSource(src);
    TelEngine::destruct(src);
}

void YBTSMedia::setConsumer(YBTSChan* chan)
{
    if (!chan)
	return;
    String format;
    getGlobalStr(format,s_format);
    YBTSDataConsumer* cons = new YBTSDataConsumer(format,chan->connId(),this);
    chan->setConsumer(cons);
    TelEngine::destruct(cons);
}

void YBTSMedia::addSource(YBTSDataSource* src)
{
    if (!src)
	return;
    Lock lck(m_srcMutex);
    ObjList* append = &m_sources;
    for (ObjList* o = m_sources.skipNull(); o; o = o->skipNext()) {
	YBTSDataSource* crt = static_cast<YBTSDataSource*>(o->get());
	if (crt->connId() < src->connId()) {
	    append = o;
	    continue;
	}
	if (src->connId() > src->connId()) {
	    o->insert(src)->setDelete(false);
	    DDebug(this,DebugInfo,"Inserted data source (%p,%u) [%p]",
		src,src->connId(),this);
	}
	else if (src != crt) {
	    o->set(src,false);
	    o->setDelete(false);
	    Debug(this,DebugInfo,"Replaced data source id=%u (%p) with (%p) [%p]",
		src->connId(),crt,src,this);
	}
	return;
    }
    append->append(src)->setDelete(false);
    DDebug(this,DebugInfo,"Added data source (%p,%u) [%p]",src,src->connId(),this);
}

void YBTSMedia::removeSource(YBTSDataSource* src)
{
    if (!src)
	return;
    Lock lck(m_srcMutex);
    if (m_sources.remove(src,false))
	DDebug(this,DebugInfo,"Removed data source (%p,%u) [%p]",src,src->connId(),this);
}

void YBTSMedia::cleanup(bool final)
{
    Lock lck(m_srcMutex);
    m_sources.clear();
}

bool YBTSMedia::start()
{
    stop();
    while (true) {
	Lock lck(this);
	if (!m_transport.initTransport(false,s_bufLenMedia,false))
	    break;
	if (!startThread("YBTSMedia"))
	    break;
	Debug(this,DebugInfo,"Started [%p]",this);
	return true;
    }
    stop();
    return false;
}

void YBTSMedia::stop()
{
    cleanup(true);
    stopThread();
    Lock lck(this);
    if (!m_transport.m_socket.valid())
	return;
    m_transport.resetTransport();
    Debug(this,DebugInfo,"Stopped [%p]",this);
}

// Read socket
void YBTSMedia::processLoop()
{
    while (__plugin.state() == YBTSDriver::WaitHandshake && !Thread::check())
	Thread::idle();
    while (!Thread::check(false)) {
	int rd = m_transport.recv();
	if (rd > 0) {
	    uint16_t* d = (uint16_t*)m_transport.m_readBuf.data();
	    YBTSDataSource* src = rd >= 2 ? find(ntohs(*d)) : 0;
	    if (!src)
		continue;
	    DataBlock tmp(d + 1,rd - 2,false);
	    src->Forward(tmp);
	    tmp.clear(false);
	    TelEngine::destruct(src);
	}
	else if (!rd) {
	    if (!m_transport.canSelect())
		Thread::idle();
	}
	else {
	    // Socket non retryable error
	    __plugin.restart();
	    break;
	}
    }
}

// Find a source object by connection id, return referrenced pointer
YBTSDataSource* YBTSMedia::find(unsigned int connId)
{
    Lock lck(m_srcMutex);
    for (ObjList* o = m_sources.skipNull(); o; o = o->skipNext()) {
	YBTSDataSource* crt = static_cast<YBTSDataSource*>(o->get());
	if (crt->connId() < connId)
	    continue;
	if (crt->connId() > connId || !crt->ref())
	    crt = 0;
	return crt;
    }
    return 0;
}


//
// YBTSUE
//
void YBTSUE::addCaller(NamedList& list)
{
    if (msisdn())
	list.addParam("caller","+" + msisdn());
    else if (imsi())
	list.addParam("caller","IMSI" + imsi());
    else if (imei())
	list.addParam("caller","IMEI" + imei());
}

// Start paging, return true if already paging
bool YBTSUE::startPaging(BtsPagingChanType type)
{
    Lock lck(this);
    m_pageCnt++;
    if (m_paging)
	return true;
    YBTSSignalling* sig = __plugin.signalling();
    if (!sig)
	return false;
    String tmp;
    if (tmsi())
	tmp << "TMSI" << tmsi();
    else if (imsi())
	tmp << "IMSI" << imsi();
    else if (imei())
	tmp << "IMEI" << imei();
    else
	return false;
    lck.drop();
    YBTSMessage m(SigStartPaging,(uint8_t)type,0,new XmlElement("identity",tmp));
    if (sig->send(m)) {
	Debug(&__plugin,DebugAll,"Started paging %s",tmp.c_str());
	lck.acquire(this);
	if (!m_paging)
	    m_paging = tmp;
	return true;
    }
    return false;
}

// Stop paging when request counter drops to zero
void YBTSUE::stopPaging()
{
    lock();
    bool stop = m_pageCnt && !--m_pageCnt;
    unlock();
    if (stop)
	stopPagingNow();
}

// Stop paging immediately
void YBTSUE::stopPagingNow()
{
    YBTSSignalling* sig = __plugin.signalling();
    if (!sig || !m_paging)
	return;
    lock();
    String tmp = m_paging;
    unlock();
    if (!tmp)
	return;
    YBTSMessage m(SigStopPaging,0,0,new XmlElement("identity",tmp));
    if (sig->send(m)) {
	Debug(&__plugin,DebugAll,"Stopped paging %s",tmp.c_str());
	lock();
	if (m_paging == tmp)
	    m_paging.clear();
	unlock();
    }
}


//
// YBTSLocationUpd
//
YBTSLocationUpd::YBTSLocationUpd(YBTSUE& ue, uint16_t connid)
    : YBTSGlobalThread("YBTSLocUpd"),
    YBTSConnIdHolder(connid),
    m_msg("user.register"),
    m_startTime(Time::now())
{
    m_imsi = ue.imsi();
    m_msg.addParam("driver",__plugin.name());
    m_msg.addParam("username",ue.imsi());
    m_msg.addParam("msisdn",ue.msisdn(),false);
    m_msg.addParam("imei",ue.imei(),false);
    m_msg.addParam("tmsi",ue.tmsi(),false);
}

void YBTSLocationUpd::run()
{
    set(this,true);
    if (!m_imsi)
	return;
    Debug(&__plugin,DebugAll,"Started location updating thread for IMSI=%s [%p]",
	m_imsi.c_str(),this);
    bool ok = Engine::dispatch(m_msg);
    if (Thread::check(false) || Engine::exiting())
	m_imsi.clear();
    notify(false,ok);
}

void YBTSLocationUpd::notify(bool final, bool ok)
{
    if (!m_imsi)
	return;
    String imsi = m_imsi;
    m_imsi.clear();
    if (!final)
	Debug(&__plugin,DebugAll,"Location updating thread for IMSI=%s terminated [%p]",
	    imsi.c_str(),this);
    else {
	ok = false;
	m_msg.setParam(s_error,String(CauseProtoError));
	if (!Engine::exiting())
	    Alarm(&__plugin,"system",DebugWarn,
		"Location updating thread for IMSI=%s abnormally terminated [%p]",
		imsi.c_str(),this);
    }
    if (__plugin.mm())
	__plugin.mm()->locUpdTerminated(m_startTime,imsi,connId(),ok,m_msg);
}


//
// YBTSSubmit
//
YBTSSubmit::YBTSSubmit(YBTSTid::Type t, uint16_t connid, YBTSUE* ue,
    const char* callRef)
    : YBTSGlobalThread("YBTSSubmit"),
    YBTSConnIdHolder(connid),
    m_type(t),
    m_callRef(callRef),
    m_msg("call.route"),
    m_ok(false),
    m_cause(111)
{
    m_msg.addParam("module",__plugin.name());
    switch (t) {
	case YBTSTid::Sms:
	    m_msg.addParam("route_type","msg");
	    break;
	case YBTSTid::Ussd:
	    m_msg.addParam("route_type","ussd");
	    break;
	default:
	    m_imsi.clear();
    }
    if (ue) {
	Lock lck(ue);
	m_imsi = ue->imsi();
	ue->addCaller(m_msg);
    }
}

void YBTSSubmit::run()
{
    set(this,true);
    Debug(&__plugin,DebugAll,
	"Started MO submit thread type=%s IMSI=%s callRef=%s [%p]",
	YBTSTid::typeName(m_type),m_imsi.c_str(),m_callRef.c_str(),this);
    while (m_imsi) {
	if (!Engine::dispatch(m_msg))
	    break;
	if (Thread::check(false) || Engine::exiting())
	    break;
	if (!m_msg.retValue() || m_msg.retValue() == YSTRING("-") ||
	    m_msg.retValue() == s_error)
	    break;
	switch (m_type) {
	    case YBTSTid::Sms:
		m_msg = "msg.execute";
	        break;
	    case YBTSTid::Ussd:
		m_msg = "ussd.execute";
	        break;
	    default:
		m_msg = "";
	}
	if (!m_msg)
	    break;
	m_msg.setParam("callto",m_msg.retValue());
	m_msg.clearParam(s_error);
	m_msg.clearParam(YSTRING("reason"));
	m_msg.retValue().clear();
	m_ok = Engine::dispatch(m_msg);
	if (m_ok)
	    m_cause = 0;
	break;
    }
    if (Thread::check(false) || Engine::exiting()) {
	m_imsi.clear();
	return;
    }
    // TODO: Try to build a cause from other param?
    if (m_type == YBTSTid::Sms) {
	m_cause = m_msg.getIntValue(s_error,m_cause,1,127);
	// TODO: Use the proper RPDU parameter name when known
	m_data = m_msg[YSTRING("irpdu")];
    }
    notify(false);
}

void YBTSSubmit::notify(bool final)
{
    if (!m_imsi)
	return;
    String imsi = m_imsi;
    m_imsi.clear();
    // Check if we restarted meanwhile
    if (!isValidStartTime(m_msg.msgTime()))
	return;
    if (!final)
	Debug(&__plugin,DebugAll,
	    "MO submit thread type=%s IMSI=%s callRef=%s terminated ok=%s data='%s' cause=%u [%p]",
	    YBTSTid::typeName(m_type),imsi.c_str(),m_callRef.c_str(),
	    String::boolText(m_ok),TelEngine::c_safe(m_data),m_cause,this);
    else if (!Engine::exiting())
	Alarm(&__plugin,"system",DebugWarn,
	    "MO submit thread type=%s IMSI=%s callRef=%s abnormally terminated [%p]",
	    YBTSTid::typeName(m_type),imsi.c_str(),m_callRef.c_str(),this);
    if (!__plugin.signalling())
	return;
    switch (m_type) {
	case YBTSTid::Sms:
	    __plugin.signalling()->moSmsRespond(connId(),m_callRef,m_cause,&m_data);
	    break;
	case YBTSTid::Ussd:
	    if (!__plugin.signalling()->moSSExecuted(connId(),m_callRef,m_ok,m_msg) &&
		m_ok) {
		Message* m = __plugin.message("ussd.finalize");
		m->copyParams(m_msg,"id,peerid");
		Engine::enqueue(m);
	    }
	    break;
	default: ;
    }
}


//
// YBTSTid
//
YBTSTid::~YBTSTid()
{
    if (m_paging && m_ue)
	m_ue->stopPaging();
    setConnId(-1);
}

// Build SS message (Facility or Release Complete)
Message* YBTSTid::ssMessage(bool facility)
{
    Message* m = 0;
    switch (m_type) {
	case Ussd:
	    m = __plugin.message(facility ? "ussd.update" : "ussd.finalize");
	    break;
	default:
	    Debug(&__plugin,DebugStub,"YBTSTid::ssMessage(): unhandled type %u",
		m_type);
	    return 0;
    }
    m->addParam("id",c_str(),false);
    m->addParam("peerid",m_peerId,false);
    return m;
}

void YBTSTid::setConnId(int connId)
{
    if (m_connId == connId)
	return;
    if (m_connId >= 0 && __plugin.signalling())
	__plugin.signalling()->setConnUsage(m_connId,false);
    m_connId = connId;
    if (m_connId >= 0 && __plugin.signalling())
	__plugin.signalling()->setConnUsage(m_connId,true);
}


//
// YBTSMtSmsList
//
// Release memory. Decrease connection usage. Stop UE paging
void YBTSMtSmsList::destroyed()
{
    stopPaging();
    if (m_conn && __plugin.signalling())
	__plugin.signalling()->setConnUsage(m_conn->connId(),false,true);
}


//
// YBTSMM
//
YBTSMM::YBTSMM(unsigned int hashLen)
    : Mutex(false,"YBTSMM"),
    m_ueMutex(false,"YBTSMMUEList"),
    m_tmsiIndex(0),
    m_ueIMSI(0),
    m_ueTMSI(0),
    m_ueHashLen(hashLen ? hashLen : 17),
    m_saveUEs(false)
{
    m_name = "ybts-mm";
    debugName(m_name);
    debugChain(&__plugin);
    m_ueIMSI = new ObjList[m_ueHashLen];
    m_ueTMSI = new ObjList[m_ueHashLen];
}

YBTSMM::~YBTSMM()
{
    delete[] m_ueIMSI;
    delete[] m_ueTMSI;
}

void YBTSMM::newTMSI(String& tmsi)
{
    do {
	uint32_t t = ++m_tmsiIndex;
	// TODO: use NNSF compatible TMSI allocation
	//  bits 31-30: 11=P-TMSI else TMSI
	//  bits 29-24: local allocation
	//  bits 23-... (0-10 bits, 0-8 for LTE compatibility): Node Identity
	//  bits ...-0 (14-24 bits, 16-24 for LTE compatibility): local allocation
	if ((t & 0xc000000) == 0xc000000)
	    m_tmsiIndex = (t &= 0x3fffffff);
	uint8_t buf[4];
	buf[0] = (uint8_t)(t >> 24);
	buf[1] = (uint8_t)(t >> 16);
	buf[2] = (uint8_t)(t >> 8);
	buf[3] = (uint8_t)t;
	tmsi.hexify(buf,4);
    } while (m_ueTMSI[hashList(tmsi)].find(tmsi));
    saveUEs();
}

void YBTSMM::locUpdTerminated(uint64_t startTime, const String& imsi, uint16_t connId,
    bool ok, const NamedList& params)
{
    if (!imsi)
	return;
    RefPointer<YBTSUE> ue;
    getUEByIMSISafe(ue,imsi,false);
    if (!ue) {
	if (ok)
	    Engine::enqueue(buildUnregister(imsi));
	return;
    }
    bool valid = isValidStartTime(startTime);
    Lock lckUE(ue);
    // IMSI already detached: revert succesful registration
    if (ue->imsiDetached()) {
	if (ok)
	    Engine::enqueue(buildUnregister(imsi,ue));
	return;
    }
    int level = DebugAll;
    if (ue->m_registered != ok)
	level = (!ok ? DebugNote : DebugInfo);
    Debug(this,level,"IMSI=%s register %s [%p]",
	ue->imsi().c_str(),ok ? "succeeded" : "failed",this);
    ue->m_registered = ok;
    ue->m_msisdn = params[YSTRING("msisdn")];
    if (ue->m_msisdn[0] == '+')
	ue->m_msisdn = ue->m_msisdn.substr(1);
    XmlElement* ch = 0;
    const char* what = ok ? "LocationUpdatingAccept" : "LocationUpdatingReject";
    XmlElement* mm = valid ? buildMM(ch,what) : 0;
    if (ch) {
	if (ok) {
	    ch->addChildSafe(__plugin.signalling()->lai().build());
	    ch->addChildSafe(buildXmlWithChild(s_mobileIdent,s_tmsi,ue->tmsi()));
	}
	else {
	    const char* cause = params.getValue(s_error,
		params.getValue(YSTRING("reason")));
	    ch->addChildSafe(new XmlElement("RejectCause",
		cause ? cause : String(CauseProtoError).c_str()));
	}
    }
    lckUE.drop();
    updateExpire(ue);
    saveUEs();
    RefPointer<YBTSConn> conn;
    if (valid && __plugin.signalling())
        __plugin.signalling()->findConn(conn,connId,false);
    if (conn) {
	conn->sendL3(mm);
	if (!ok)
	    __plugin.signalling()->dropConn(connId,true);
	return;
    }
    if (valid)
	Debug(this,DebugNote,"Can't send '%s' for IMSI=%s: connection %u vanished [%p]",
	    what,imsi.c_str(),connId,this);
    else
	Debug(this,DebugNote,"Can't send '%s' for IMSI=%s: restarted meanwhile [%p]",
	    what,imsi.c_str(),this);
    TelEngine::destruct(mm);
}

void YBTSMM::loadUElist()
{
    s_globalMutex.lock();
    String f = s_ueFile;
    s_globalMutex.unlock();
    if (!f)
	return;
    Configuration ues(f);
    if (!ues.load(false))
	return;
    m_tmsiIndex = ues.getIntValue("tmsi","index");
    NamedList* tmsis = ues.getSection("ues");
    if (!tmsis)
	return;
    int cnt = 0;
    for (ObjList* l = tmsis->paramList()->skipNull(); l; l = l->skipNext()) {
	const NamedString* s = static_cast<const NamedString*>(l->get());
	if (s->name().length() != 8) {
	    Debug(this,DebugMild,"Invalid TMSI '%s' in file '%s'",s->name().c_str(),f.c_str());
	    continue;
	}
	ObjList* p = s->split(',');
	if (p && (p->count() >= 5) && p->at(0)->toString()) {
	    YBTSUE* ue = new YBTSUE(p->at(0)->toString(),s->name());
	    ue->m_imei = p->at(1)->toString();
	    ue->m_msisdn = p->at(2)->toString();
	    ue->m_expires = p->at(3)->toString().toInt64();
	    ue->m_registered = p->at(4)->toString().toBoolean();
	    m_ueIMSI[hashList(ue->imsi())].append(ue);
	    m_ueTMSI[hashList(ue->tmsi())].append(ue)->setDelete(false);
	    cnt++;
	}
	else
	    Debug(this,DebugMild,"Invalid record for TMSI '%s' in file '%s'",s->name().c_str(),f.c_str());
	TelEngine::destruct(p);
    }
    Debug(this,DebugNote,"Loaded %d TMSI records, index=%u",cnt,m_tmsiIndex);
}

void YBTSMM::saveUElist()
{
    s_globalMutex.lock();
    String f = s_ueFile;
    s_globalMutex.unlock();
    if (!f)
	return;
    Configuration ues;
    ues = f;
    ues.setValue(YSTRING("tmsi"),"index",(int)m_tmsiIndex);
    int cnt = 0;
    if (s_tmsiSave) {
	NamedList* tmsis = ues.createSection(YSTRING("ues"));
	ObjList* list = tmsis->paramList();
	for (unsigned int i = 0; i < m_ueHashLen; i++) {
	    for (ObjList* l = m_ueTMSI[i].skipNull(); l; l = l->skipNext()) {
		YBTSUE* ue = static_cast<YBTSUE*>(l->get());
		String s;
		s << ue->imsi() << "," << ue->imei() << "," << ue->msisdn() << "," << ue->expires() << "," << ue->registered();
		list = list->append(new NamedString(ue->tmsi(),s));
		cnt++;
	    }
	}
    }
    ues.save();
    Debug(this,DebugNote,"Saved %d TMSI records, index=%u",cnt,m_tmsiIndex);
}

Message* YBTSMM::buildUnregister(const String& imsi, YBTSUE* ue)
{
    if (!imsi)
	return 0;
    Message* m = new Message("user.unregister");
    m->addParam("driver",__plugin.name());
    m->addParam("username",imsi);
    if (ue) {
	m->addParam("msisdn",ue->msisdn(),false);
	m->addParam("imei",ue->imei(),false);
	m->addParam("tmsi",ue->tmsi(),false);
    }
    return m;
}

void YBTSMM::updateExpire(YBTSUE* ue)
{
    uint32_t exp = s_tmsiExpire;
    if (!ue || !exp)
	return;
    exp += Time::secNow();
    Lock lck(ue);
    if (exp <= ue->expires())
	return;
    ue->m_expires = exp;
    DDebug(this,DebugAll,"Updated TMSI=%s IMSI=%s expiration time",ue->tmsi().c_str(),ue->imsi().c_str());
    lck.drop();
    saveUEs();
}

void YBTSMM::checkTimers(const Time& time)
{
    uint32_t exp = time.sec();
    Lock mylock(m_ueMutex);
    for (unsigned int i = 0; i < m_ueHashLen; i++) {
	for (ObjList* l = m_ueTMSI[i].skipNull(); l; ) {
	    YBTSUE* ue = static_cast<YBTSUE*>(l->get());
	    if (ue->expires() && ue->expires() < exp) {
		m_ueIMSI[hashList(ue->imsi())].remove(ue,false);
		l->remove(false);
		l = l->skipNull();
		saveUEs();
		ueRemoved(*ue,"expired");
		TelEngine::destruct(ue);
	    }
	    else
		l = l->skipNext();
	}
    }
    if (m_saveUEs) {
	m_saveUEs = false;
	saveUElist();
    }
}

void YBTSMM::handlePDU(YBTSMessage& m, YBTSConn* conn)
{
    XmlElement* ch = m.xml() ? m.xml()->findFirstChild(&s_message) : 0;
    if (!ch) {
	Debug(this,DebugNote,"Empty xml in %s [%p]",m.name(),this);
	return;
    }
    const String* t = ch->getAttribute(s_type);
    if (!t)
	Debug(this,DebugWarn,"Missing 'type' in %s [%p]",m.name(),this);
    else if (*t == YSTRING("LocationUpdatingRequest"))
	handleLocationUpdate(m,*ch,conn);
    else if (*t == YSTRING("TMSIReallocationComplete"))
	handleUpdateComplete(m,*ch,conn);
    else if (*t == YSTRING("IMSIDetachIndication"))
	handleIMSIDetach(m,*ch,conn);
    else if (*t == YSTRING("CMServiceRequest"))
	handleCMServiceRequest(m,*ch,conn);
    else if (*t == YSTRING("CMServiceAbort"))
	__plugin.signalling()->dropConn(m.connId(),true);
    else if (*t == YSTRING("IdentityResponse"))
	handleIdentityResponse(m,*ch,conn);
    else {
	if (*t != YSTRING("MMStatus")) {
	    Debug(this,DebugNote,"Unhandled '%s' in %s [%p]",t->c_str(),m.name(),this);
	    XmlElement* ch = 0;
	    XmlElement* mm = buildMM(ch,"MMStatus");
	    if (ch)
		ch->addChildSafe(new XmlElement("RejectCause",String(CauseUnknownMsg)));
	    __plugin.signalling()->sendL3Conn(m.connId(),mm);
	}
	else {
	    const String* cause = ch->childText(YSTRING("RejectCause"));
	    Debug(this,DebugInfo,"Received %s cause='%s' [%p]",
		t->c_str(),TelEngine::c_safe(cause),this);
	}
    }
}

bool YBTSMM::handlePagingResponse(YBTSMessage& m, YBTSConn* conn, XmlElement& rsp)
{
    XmlElement* x = rsp.findFirstChild(&s_mobileIdent);
    x = x ? x->findFirstChild() : 0;
    if (!x) {
	Debug(this,DebugInfo,"PagingResponse with no identity on conn=%u [%p]",
	    m.connId(),this);
	__plugin.signalling()->sendRRMStatus(m.connId(),CauseInvalidIE);
	return false;
    }
    const String& type = x->getTag();
    const String& ident = x->getText();
    if (!ident) {
	Debug(this,DebugNote,"PagingResponse with empty identity '%s' conn=%u [%p]",
	    type.c_str(),m.connId(),this);
	__plugin.signalling()->sendRRMStatus(m.connId(),CauseInvalidIE);
	return false;
    }
    String paging = type + ident;
    RefPointer<YBTSUE> ue;
    if (!findUEPagingSafe(ue,paging)) {
	Debug(this,DebugNote,"PagingResponse %s=%s conn=%u for unknown UE [%p]",
	    type.c_str(),ident.c_str(),m.connId(),this);
	__plugin.signalling()->sendRRMStatus(m.connId(),CauseUnexpectedMsg);
	return false;
    }
    Debug(this,DebugAll,"PagingResponse with %s=%s conn=%u [%p]",
	type.c_str(),ident.c_str(),m.connId(),this);
    ue->stopPagingNow();
    if (conn && setConnUE(*conn,ue,rsp) != 0)
	conn = 0;
    // Always notify paging response even if something wetn wrong with the connection
    // This will allow entities waiting for paging response to stop waiting
    __plugin.checkMtService(ue,conn,true);
    return true;
}

void YBTSMM::handleIdentityResponse(YBTSMessage& m, const XmlElement& xml, YBTSConn* conn)
{
    if (!conn) {
	Debug(this,DebugGoOn,"Oops! IdentityResponse conn=%u: no connection [%p]",
	    m.connId(),this);
	__plugin.signalling()->dropConn(m.connId(),true);
	return;
    }
    XmlElement* xid = xml.findFirstChild(&s_mobileIdent);
    if (!xid)
	return;
    xid = xid->findFirstChild();
    if (!xid)
	return;
    const String& tag = xid->getTag();
    const String& ident = xid->getText();
    RefPointer<YBTSUE> ue = conn->ue();
    if (tag == YSTRING("IMSI")) {
	if (ue) {
	    if (ue->imsi() != ident) {
		Debug(this,DebugWarn,"Got IMSI change %s -> %s on conn=%u [%p]",
		    ue->imsi().c_str(),ident.c_str(),m.connId(),this);
		__plugin.signalling()->dropConn(conn,true);
	    }
	}
	else {
	    getUEByIMSISafe(ue,ident);
	    conn->lock();
	    bool ok = conn->setUE(ue);
	    conn->unlock();
	    if (!ok) {
		Debug(this,DebugGoOn,"Failed to set UE in conn=%u [%p]",m.connId(),this);
		__plugin.signalling()->dropConn(conn,true);
		return;
	    }
	}
    }
    else {
	if (!ue) {
	    Debug(this,DebugWarn,"Got identity %s=%s but have no UE attached on conn=%u [%p]",
		tag.c_str(),ident.c_str(),m.connId(),this);
	    __plugin.signalling()->dropConn(conn,true);
	    return;
	}
    }
    if (tag == YSTRING("IMEI"))
	ue->m_imei = ident;
    XmlElement* x = conn->takeXml();
    if (x) {
	YBTSMessage m2(SigL3Message,m.info(),m.connId(),x);
	handlePDU(m2,conn);
    }
}

// Handle location updating requests
void YBTSMM::handleLocationUpdate(YBTSMessage& m, const XmlElement& xml, YBTSConn* conn)
{
    if (!conn) {
	Debug(this,DebugGoOn,"Rejecting LocationUpdatingRequest conn=%u: no connection [%p]",
	    m.connId(),this);
	sendLocationUpdateReject(m,0,CauseProtoError);
	return;
    }
    RefPointer<YBTSUE> ue = conn->ue();
    if (!ue) {
	XmlElement* laiXml = 0;
	XmlElement* identity = 0;
	findXmlChildren(xml,laiXml,s_locAreaIdent,identity,s_mobileIdent);
	if (!(laiXml && identity)) {
	    Debug(this,DebugNote,
		"Rejecting LocationUpdatingRequest conn=%u: missing LAI or mobile identity [%p]",
		m.connId(),this);
	    sendLocationUpdateReject(m,conn,CauseInvalidIE);
	    return;
	}
	bool haveTMSI = false;
	const String* ident = 0;
	uint8_t cause = getMobileIdentTIMSI(m,xml,*identity,ident,haveTMSI);
	if (cause) {
	    sendLocationUpdateReject(m,conn,cause);
	    return;
	}
	YBTSLAI lai(*laiXml);
	bool haveLAI = (lai == __plugin.signalling()->lai());
	Debug(this,DebugAll,"Handling LocationUpdatingRequest conn=%u: ident=%s/%s LAI=%s [%p]",
	    conn->connId(),(haveTMSI ? "TMSI" : "IMSI"),
	    ident->c_str(),lai.lai().c_str(),this);
	// TODO: handle concurrent requests, check if we have a pending location updating
	// This should never happen, but we should handle it
	if (haveTMSI) {
	    // Got TMSI
	    // Same LAI: check if we know the IMSI, request it if unknown
	    // Different LAI: request IMSI
	    if (haveLAI)
		findUEByTMSISafe(ue,*ident);
	}
	else
	    // Got IMSI: Create/retrieve TMSI
	    getUEByIMSISafe(ue,*ident);
	if (!ue) {
	    if (!conn->setXml(m.takeXml())) {
		Debug(this,DebugNote,"Rejecting LocationUpdatingRequest: cannot postpone request [%p]",this);
		sendLocationUpdateReject(m,conn,CauseServNotSupp);
		return;
	    }
	    sendIdentityRequest(conn,"IMSI");
	    return;
	}
	conn->lock();
	cause = setConnUE(*conn,ue,xml);
	conn->unlock();
	if (cause) {
	    sendLocationUpdateReject(m,conn,cause);
	    return;
	}
    }
    Lock lckUE(ue);
    if (s_askIMEI && ue->imei().null()) {
	if (conn->setXml(m.takeXml())) {
	    lckUE.drop();
	    sendIdentityRequest(conn,"IMEI");
	    return;
	}
    }
    ue->m_imsiDetached = false;
    YBTSLocationUpd* th = new YBTSLocationUpd(*ue,conn->connId());
    lckUE.drop();
    if (!th->startup()) {
	delete th;
	ue->lock();
	Debug(this,DebugNote,"Location updating for IMSI=%s: failed to start thread [%p]",
	    ue->imsi().c_str(),this);
	ue->unlock();
	sendLocationUpdateReject(m,conn,CauseProtoError);
    }
    updateExpire(ue);
    saveUEs();
}

// Handle location update (TMSI reallocation) complete
void YBTSMM::handleUpdateComplete(YBTSMessage& m, const XmlElement& xml, YBTSConn* conn)
{
    if (!conn) {
	Debug(this,DebugGoOn,"Rejecting TMSIReallocationComplete conn=%u: no connection [%p]",
	    m.connId(),this);
	sendLocationUpdateReject(m,0,CauseProtoError);
	return;
    }
    __plugin.signalling()->dropConn(conn,true);
}

// Handle IMSI detach indication
void YBTSMM::handleIMSIDetach(YBTSMessage& m, const XmlElement& xml, YBTSConn* conn)
{
    if (!conn) {
	Debug(this,DebugGoOn,"Ignoring IMSIDetachIndication conn=%u: no connection [%p]",
	    m.connId(),this);
	return;
    }
    __plugin.signalling()->dropConn(conn,true);

    const XmlElement* xid = xml.findFirstChild(&s_mobileIdent);
    if (!xid)
	return;
    bool found = false;
    bool haveTMSI = false;
    const String* ident = getIdentTIMSI(*xid,haveTMSI,found);
    if (TelEngine::null(ident))
	return;

    RefPointer<YBTSUE> ue;
    if (haveTMSI)
	findUEByTMSISafe(ue,*ident);
    else
	getUEByIMSISafe(ue,*ident,false);
    if (!ue)
	return;
    ue->stopPagingNow();
    Lock lckUE(ue);
    if (!ue->imsiDetached()) {
	Debug(this,DebugInfo,"Detached IMSI=%s [%p]",ue->imsi().c_str(),this);
	ue->m_imsiDetached = true;
    }
    if (!ue->registered())
	return;
    ue->m_registered = false;
    Message* msg = buildUnregister(ue->imsi(),ue);
    lckUE.drop();
    updateExpire(ue);
    saveUEs();
    if (msg)
	Engine::enqueue(msg);
}

void YBTSMM::handleCMServiceRequest(YBTSMessage& m, const XmlElement& xml, YBTSConn* conn)
{
    if (!conn) {
	Debug(this,DebugGoOn,"Rejecting CMServiceRequest conn=%u: no connection [%p]",
	    m.connId(),this);
	sendCMServiceRsp(m,0,CauseProtoError);
	return;
    }
    RefPointer<YBTSUE> ue = conn->ue();
    if (!ue) {
	XmlElement* cmServType = 0;
	XmlElement* identity = 0;
	findXmlChildren(xml,cmServType,s_cmServType,identity,s_mobileIdent);
	if (!(cmServType && identity)) {
	    Debug(this,DebugNote,
		"Rejecting CMServiceRequest conn=%u: missing service type or mobile identity [%p]",
		m.connId(),this);
	    sendCMServiceRsp(m,conn,CauseInvalidIE);
	    return;
	}
	const String& type = cmServType->getText();
	if (type == s_cmMOCall || type == s_cmSMS || type == s_cmSS)
	    ;
	else {
	    Debug(this,DebugNote,
		"Rejecting CMServiceRequest conn=%u: service type '%s' not supported/subscribed [%p]",
		m.connId(),type.c_str(),this);
	    sendCMServiceRsp(m,conn,CauseServNotSupp);
	    return;
	}
	bool haveTMSI = false;
	const String* ident = 0;
	uint8_t cause = getMobileIdentTIMSI(m,xml,*identity,ident,haveTMSI);
	if (cause) {
	    sendCMServiceRsp(m,conn,cause);
	    return;
	}
	Debug(this,DebugAll,"Handling CMServiceRequest conn=%u: ident=%s/%s type=%s [%p]",
	    conn->connId(),(haveTMSI ? "TMSI" : "IMSI"),
	    ident->c_str(),type.c_str(),this);
	if (haveTMSI)
	    // Got TMSI
	    findUEByTMSISafe(ue,*ident);
	else
	    // Got IMSI: Create/retrieve TMSI
	    getUEByIMSISafe(ue,*ident);
	if (!ue) {
	    if (!conn->setXml(m.takeXml())) {
		Debug(this,DebugNote,"Rejecting CMServiceRequest: cannot postpone request [%p]",this);
		sendCMServiceRsp(m,conn,CauseServNotSupp);
	    }
	    sendIdentityRequest(conn,"IMSI");
	    return;
	}
	bool dropConn = false;
	conn->lock();
	cause = setConnUE(*conn,ue,xml,&dropConn);
	conn->unlock();
	if (cause) {
	    sendCMServiceRsp(m,conn,cause);
	    if (dropConn)
		__plugin.signalling()->dropConn(conn,true);
	    return;
	}
    }
    ue->stopPagingNow();
    sendCMServiceRsp(m,conn,0);
    __plugin.checkMtService(ue,conn);
}

void YBTSMM::sendLocationUpdateReject(YBTSMessage& msg, YBTSConn* conn, uint8_t cause)
{
    XmlElement* ch = 0;
    XmlElement* mm = buildMM(ch,"LocationUpdatingReject");
    if (ch)
	ch->addChildSafe(new XmlElement("RejectCause",String(cause)));
    if (conn)
	conn->sendL3(mm);
    else
	__plugin.signalling()->sendL3Conn(msg.connId(),mm);
    __plugin.signalling()->dropConn(msg.connId(),true);
}

void YBTSMM::sendCMServiceRsp(YBTSMessage& msg, YBTSConn* conn, uint8_t cause)
{
    XmlElement* ch = 0;
    XmlElement* mm = buildMM(ch,!cause ? "CMServiceAccept" : "CMServiceReject");
    if (ch && cause)
	ch->addChildSafe(new XmlElement("RejectCause",String(cause)));
    if (conn)
	conn->sendL3(mm);
    else
	__plugin.signalling()->sendL3Conn(msg.connId(),mm);
}

// Send an Identity Request message
void YBTSMM::sendIdentityRequest(YBTSConn* conn, const char* type)
{
    if (TelEngine::null(type) || !conn)
	return;
    XmlElement* ch = 0;
    XmlElement* mm = buildMM(ch,"IdentityRequest");
    if (ch)
	ch->addChildSafe(new XmlElement("IdentityType",type));
    conn->sendL3(mm);
}

bool YBTSMM::findUESafe(RefPointer<YBTSUE>& ue, const String& dest)
{
    ue = 0;
    String tmp(dest);
    if (tmp.startSkip("+",false))
	findUEByMSISDNSafe(ue,tmp);
    else if (tmp.startSkip("IMSI",false))
	getUEByIMSISafe(ue,tmp,false);
    else if (tmp.startSkip("IMEI",false))
	findUEByIMEISafe(ue,tmp);
    else if (tmp.startSkip("TMSI",false))
	findUEByTMSISafe(ue,tmp);
    return ue != 0;
}

// Find UE by paging identity
bool YBTSMM::findUEPagingSafe(RefPointer<YBTSUE>& ue, const String& paging)
{
    if (!paging)
	return false;
    Lock lck(m_ueMutex);
    for (unsigned int n = 0; n < m_ueHashLen; n++) {
	ObjList& list = m_ueTMSI[n];
	for (ObjList* o = list.skipNull(); o ; o = o->skipNext()) {
	    YBTSUE* u = static_cast<YBTSUE*>(o->get());
	    Lock lckUE(u);
	    if (paging == u->paging()) {
		ue = u;
		return (ue != 0);
	    }
	}
    }
    return false;
}

// Find UE by TMSI
void YBTSMM::findUEByTMSISafe(RefPointer<YBTSUE>& ue, const String& tmsi)
{
    if (!tmsi)
	return;
    Lock lck(m_ueMutex);
    YBTSUE* tmpUE = 0;
    ObjList& list = m_ueTMSI[hashList(tmsi)];
    for (ObjList* o = list.skipNull(); o ; o = o->skipNext()) {
	YBTSUE* u = static_cast<YBTSUE*>(o->get());
	if (tmsi == u->tmsi()) {
	    tmpUE = u;
	    break;
	}
    }
    XDebug(this,DebugAll,"findUEByTMSISafe(%s) found (%p) [%p]",
	tmsi.c_str(),tmpUE,this);
    ue = tmpUE;
}

// Find UE by IMEI
void YBTSMM::findUEByIMEISafe(RefPointer<YBTSUE>& ue, const String& imei)
{
    if (!imei)
	return;
    Lock lck(m_ueMutex);
    for (unsigned int n = 0; n < m_ueHashLen; n++) {
	ObjList& list = m_ueTMSI[n];
	for (ObjList* o = list.skipNull(); o ; o = o->skipNext()) {
	    YBTSUE* u = static_cast<YBTSUE*>(o->get());
	    if (imei == u->imei()) {
		ue = u;
		return;
	    }
	}
    }
}

// Find UE by MSISDN
void YBTSMM::findUEByMSISDNSafe(RefPointer<YBTSUE>& ue, const String& msisdn)
{
    if (!msisdn)
	return;
    Lock lck(m_ueMutex);
    for (unsigned int n = 0; n < m_ueHashLen; n++) {
	ObjList& list = m_ueTMSI[n];
	for (ObjList* o = list.skipNull(); o ; o = o->skipNext()) {
	    YBTSUE* u = static_cast<YBTSUE*>(o->get());
	    if (msisdn == u->msisdn()) {
		ue = u;
		return;
	    }
	}
    }
}

// Find UE by IMSI. Create it if not found
void YBTSMM::getUEByIMSISafe(RefPointer<YBTSUE>& ue, const String& imsi, bool create)
{
    if (!imsi)
	return;
    Lock lck(m_ueMutex);
    YBTSUE* tmpUE = 0;
    ObjList& list = m_ueIMSI[hashList(imsi)];
    for (ObjList* o = list.skipNull(); o ; o = o->skipNext()) {
	YBTSUE* u = static_cast<YBTSUE*>(o->get());
	if (imsi == u->imsi()) {
	    tmpUE = u;
	    break;
	}
    }
    if (create && !tmpUE) {
	String tmsi;
	newTMSI(tmsi);
	tmpUE = new YBTSUE(imsi,tmsi);
	list.append(tmpUE);
	m_ueTMSI[hashList(tmsi)].append(tmpUE)->setDelete(false);
	Debug(this,DebugInfo,"Added UE IMSI=%s TMSI=%s [%p]",
	    tmpUE->imsi().c_str(),tmpUE->tmsi().c_str(),this);
	saveUEs();
    }
    ue = tmpUE;
}

// Get IMSI/TMSI from request
uint8_t YBTSMM::getMobileIdentTIMSI(YBTSMessage& m, const XmlElement& request,
    const XmlElement& identXml, const String*& ident, bool& isTMSI)
{
    bool found = false;
    ident = getIdentTIMSI(identXml,isTMSI,found);
    if (ident)
	return 0;
    const String* type = request.getAttribute(s_type);
    Debug(this,DebugNote,"Rejecting %s conn=%u: %s IMSI/TMSI [%p]",
	(type ? type->c_str() : "unknown"),m.connId(),(found ? "empty" : "missing"),this);
    return CauseInvalidIE;
}

// Set UE for a connection
uint8_t YBTSMM::setConnUE(YBTSConn& conn, YBTSUE* ue, const XmlElement& req,
    bool* dropConn)
{
    const String* type = req.getAttribute(s_type);
    if (!ue) {
	Debug(this,DebugGoOn,"Rejecting %s: no UE object [%p]",
	    (type ? type->c_str() : "unknown"),this);
	return CauseProtoError;
    }
    if (conn.setUE(ue))
	return 0;
    Debug(this,DebugGoOn,"Rejecting %s: UE mismatch on connection %u [%p]",
	(type ? type->c_str() : "unknown"),conn.connId(),this);
    if (dropConn)
	*dropConn = true;
    return CauseProtoError;
}

void YBTSMM::ueRemoved(YBTSUE& ue, const char* reason)
{
    Lock lck(ue);
    ue.m_removed = true;
    Debug(this,DebugInfo,"Removed UE IMSI=%s TMSI=%s: %s [%p]",
	ue.imsi().c_str(),ue.tmsi().c_str(),reason,this);
}

//
// YBTSCallDesc
//
// Incoming
YBTSCallDesc::YBTSCallDesc(YBTSChan* chan, const XmlElement& xml, bool regular, const String* callRef)
    : YBTSConnIdHolder(chan->connId()),
    m_state(Null),
    m_incoming(true),
    m_regular(regular),
    m_timeout(0),
    m_relSent(0),
    m_chan(chan)
{
    __plugin.signalling()->setConnUsage(connId(),true);
    if (callRef) {
	m_callRef = *callRef;
	*this << prefix(false) << m_callRef;
    }
    for (const ObjList* o = xml.getChildren().skipNull(); o; o = o->skipNext()) {
	XmlElement* x = static_cast<XmlChild*>(o->get())->xmlElement();
	if (!x)
	    continue;
	const String& s = x->getTag();
	if (s == s_ccCalled)
	    m_called = x->getText();
    }
}

// Outgoing
YBTSCallDesc::YBTSCallDesc(YBTSChan* chan, const ObjList& xml, const String& callRef)
    : YBTSConnIdHolder(chan->connId()),
    m_state(Null),
    m_incoming(false),
    m_regular(true),
    m_timeout(0),
    m_relSent(0),
    m_chan(chan),
    m_callRef(callRef)
{
    __plugin.signalling()->setConnUsage(connId(),true);
    *this << prefix(true) << m_callRef;
    // TODO: pick relevant info from XML list
}

YBTSCallDesc::~YBTSCallDesc()
{
    if (__plugin.signalling())
	__plugin.signalling()->setConnUsage(connId(),false);
}

void YBTSCallDesc::proceeding()
{
    changeState(CallProceeding);
    sendCC(s_ccProceeding);
}

void YBTSCallDesc::progressing(XmlElement* indicator)
{
    sendCC(s_ccProgress,indicator);
}

void YBTSCallDesc::alert(XmlElement* indicator)
{
    changeState(CallDelivered);
    sendCC(s_ccAlerting,indicator);
}

void YBTSCallDesc::connect(XmlElement* indicator)
{
    changeState(ConnectReq);
    sendCC(s_ccConnect,indicator);
}

void YBTSCallDesc::hangup()
{
    changeState(Disconnect);
    sendCC(s_ccDisc,buildCCCause(reason()));
}

void YBTSCallDesc::sendStatus(const char* cause)
{
    sendCC(s_ccStatus,buildCCCause(cause),buildCCCallState(m_state));
}

bool YBTSCallDesc::sendCC(const String& tag, XmlElement* c1, XmlElement* c2)
{
    XmlElement* ch = 0;
    XmlElement* cc = __plugin.buildCC(ch,tag,callRef(),m_incoming);
    if (ch) {
	ch->addChildSafe(c1);
	ch->addChildSafe(c2);
    }
    else {
	TelEngine::destruct(c1);
	TelEngine::destruct(c2);
    }
    return __plugin.signalling()->sendL3Conn(connId(),cc);
}

bool YBTSCallDesc::sendCC(const String& tag, ObjList& children)
{
    XmlElement* ch = 0;
    XmlElement* cc = __plugin.buildCC(ch,tag,callRef(),m_incoming);
    for (ObjList* o = ch ? children.skipNull() : 0; o; o = o->skipNull()) {
	XmlElement* x = static_cast<XmlElement*>(o->remove(false));
	ch->addChildSafe(x);
    }
    return __plugin.signalling()->sendL3Conn(connId(),cc);
}

void YBTSCallDesc::changeState(int newState)
{
    if (m_state == newState)
	return;
    if (m_chan)
	Debug(m_chan,DebugInfo,"Call '%s' changed state %s -> %s [%p]",
	    c_str(),stateName(),stateName(newState),m_chan);
    else
	Debug(&__plugin,DebugInfo,"Call '%s' changed state %s -> %s",
	    c_str(),stateName(),stateName(newState));
    m_state = newState;
}

// Send CC REL or RLC
void YBTSCallDesc::sendGSMRel(bool rel, const String& callRef, bool tiFlag, const char* reason,
    uint16_t connId)
{
    XmlElement* ch = 0;
    XmlElement* cc = __plugin.buildCC(ch,rel ? s_ccRel : s_rlc,callRef,tiFlag);
    if (ch)
	ch->addChildSafe(buildCCCause(reason));
    __plugin.signalling()->sendL3Conn(connId,cc);
}


//
// YBTSChan
//
// Incoming
YBTSChan::YBTSChan(YBTSConn* conn)
    : Channel(__plugin),
    YBTSConnIdHolder(conn->connId()),
    m_mutex(true,"YBTSChan"),
    m_conn(conn),
    m_waitForTraffic(false),
    m_cref(0),
    m_dtmf(0),
    m_mpty(false),
    m_hungup(false),
    m_paging(false),
    m_haveTout(false),
    m_tout(0),
    m_activeCall(0),
    m_pending(0),
    m_route(0)
{
    if (!m_conn)
	return;
    __plugin.signalling()->setConnUsage(connId(),true);
    m_address = m_conn->ue()->imsi();
    Debug(this,DebugCall,"Incoming imsi=%s conn=%u [%p]",
	m_address.c_str(),connId(),this);
}

// Outgoing
YBTSChan::YBTSChan(YBTSUE* ue, YBTSConn* conn)
    : Channel(__plugin,0,true),
    m_mutex(true,"YBTSChan"),
    m_conn(conn),
    m_ue(ue),
    m_waitForTraffic(false),
    m_cref(0),
    m_dtmf(0),
    m_mpty(false),
    m_hungup(false),
    m_paging(false),
    m_haveTout(false),
    m_tout(0),
    m_activeCall(0),
    m_pending(0),
    m_route(0)
{
    if (m_conn)
	__plugin.signalling()->setConnUsage(connId(),true);
    if (!m_ue)
	return;
    m_address = ue->imsi();
    Debug(this,DebugCall,"Outgoing imsi=%s [%p]",m_address.c_str(),this);
}

// Init incoming chan. Return false to destruct the channel
bool YBTSChan::initIncoming(const XmlElement& xml, bool regular, const String* callRef)
{
    if (!ue())
	return false;
    Lock lck(driver());
    YBTSCallDesc* call = handleSetup(xml,regular,callRef);
    if (!call)
	return false;
    m_route = message("call.preroute");
    // Keep the channel alive if not routing
    if (m_waitForTraffic)
	m_route->userData(this);
    ue()->lock();
    ue()->addCaller(*m_route);
    ue()->unlock();
    m_route->addParam("called",call->m_called,false);
    m_route->addParam("username",ue()->imsi(),false);
    m_route->addParam("imei",ue()->imei(),false);
    m_route->addParam("emergency",String::boolText(!call->m_regular));
    if (xml.findFirstChild(&s_ccSsCLIR))
	m_route->addParam("privacy",String::boolText(true));
    else if (xml.findFirstChild(&s_ccSsCLIP))
	m_route->addParam("privacy",String::boolText(false));
    Message* s = message("chan.startup");
    s->addParam("caller",m_route->getValue(YSTRING("caller")),false);
    s->addParam("called",call->m_called,false);
    lck.drop();
    Engine::enqueue(s);
    initChan();
    return m_waitForTraffic ? true : route();
}

// Init outgoing chan. Return false to destruct the channel
bool YBTSChan::initOutgoing(Message& msg)
{
    if (m_pending || !ue())
	return false;
    Lock lck(m_mutex);
    if (m_pending)
	return false;
    m_pending = new ObjList;
    if (!msg.getBoolValue(YSTRING("privacy")));
	addXmlFromParam(*m_pending,msg,"CallingPartyBCDNumber",YSTRING("caller"));
    lck.drop();
    if (!m_conn)
	m_paging = ue()->startPaging(ChanTypeVoice);
    initChan();
    startMT();
    return true;
}

// Handle CC messages
bool YBTSChan::handleCC(const XmlElement& xml, const String* callRef, bool tiFlag)
{
    const String* type = xml.getAttribute(s_type);
    if (!type) {
	Debug(this,DebugWarn,"Missing 'type' attribute [%p]",this);
	return true;
    }
    if (TelEngine::null(callRef)) {
	Debug(this,DebugNote,"%s with empty transaction identifier [%p]",
	    type->c_str(),this);
	return true;
    }
    bool regular = (*type == s_ccSetup);
    bool emergency = !regular && (*type == s_ccEmergency);
    Lock lck(m_mutex);
    String cref;
    cref << YBTSCallDesc::prefix(tiFlag) << *callRef;
    ObjList* o = m_calls.find(cref);
    DDebug(this,DebugAll,"Handling '%s' in call %s (%p) [%p]",type->c_str(),cref.c_str(),o,this);
    if (!o) {
	lck.drop();
	if (regular || emergency) {
	    handleSetup(xml,regular,callRef);
	    return true;
	}
	return false;
    }
    YBTSCallDesc* call = static_cast<YBTSCallDesc*>(o->get());
    if (regular || emergency)
	call->sendWrongState();
    else if (*type == s_ccConnect) {
	if (call->m_state == YBTSCallDesc::CallConfirmed ||
	    call->m_state == YBTSCallDesc::CallReceived) {
	    call->sendCC(s_ccConnectAck);
	    call->changeState(YBTSCallDesc::Active);
	    Engine::enqueue(message("call.answered"));
	}
	else
	    call->sendWrongState();
    }
    else if (*type == s_ccAlerting) {
	if (call->m_state == YBTSCallDesc::CallConfirmed) {
	    call->changeState(YBTSCallDesc::CallReceived);
	    Message* m = message("call.ringing");
	    // TODO: set early media
	    Engine::enqueue(m);
	}
	else
	    call->sendWrongState();
    }
    else if (*type == s_ccConfirmed) {
	if (call->m_state == YBTSCallDesc::CallPresent) {
	    call->changeState(YBTSCallDesc::CallConfirmed);
	    Engine::enqueue(message("call.progress"));
	}
	else
	    call->sendWrongState();
    }
    else if (*type == s_ccRel || *type == s_rlc || *type == s_ccDisc) {
	Debug(this,DebugInfo,"Removing call '%s' [%p]",call->c_str(),this);
	if (m_activeCall == call)
	    m_activeCall = 0;
	getCCCause(call->m_reason,xml);
	String reason = call->m_reason;
	bool final = (*type != s_ccDisc);
	if (final) {
	    if (*type == s_ccRel)
		call->releaseComplete();
	    else
		call->changeState(YBTSCallDesc::Null);
	}
	else {
	    call->release();
	    call->setTimeout(s_t308);
	}
	call = static_cast<YBTSCallDesc*>(o->remove(final));
	bool disc = (m_calls.skipNull() == 0);
	lck.drop();
	if (call)
	    __plugin.addTerminatedCall(call);
	if (disc)
	    hangup(reason);
    }
    else if (*type == s_ccConnectAck) {
	if (call->m_state == YBTSCallDesc::ConnectReq) {
	    call->changeState(YBTSCallDesc::Active);
	    call->m_timeout = 0;
	}
	else
	    call->sendWrongState();
    }
    else if (*type == s_ccStatusEnq)
	call->sendStatus("status-enquiry-rsp");
    else if (*type == s_ccStatus) {
	String cause, cs;
	getCCCause(cause,xml);
	getCCCallState(cs,xml);
	Debug(this,(cause != "status-enquiry-rsp") ? DebugWarn : DebugAll,
	    "Received status cause='%s' call_state='%s' [%p]",
	    cause.c_str(),cs.c_str(),this);
    }
    else if (*type == s_ccStartDTMF) {
	const String* dtmf = xml.childText(s_ccKeypadFacility);
	if (m_dtmf) {
	    Debug(this,DebugMild,"Received DTMF '%s' while still in '%c' [%p]",
		TelEngine::c_str(dtmf),m_dtmf,this);
	    call->sendCC(YSTRING("StartDTMFReject"),buildCCCause("wrong-state-message"));
	}
	else if (TelEngine::null(dtmf)) {
	    Debug(this,DebugMild,"Received no %s in %s [%p]",
		s_ccKeypadFacility.c_str(),s_ccStartDTMF.c_str(),this);
	    call->sendCC(YSTRING("StartDTMFReject"),buildCCCause("missing-mandatory-ie"));
	}
	else {
	    m_dtmf = dtmf->at(0);
	    call->sendCC(YSTRING("StartDTMFAck"),new XmlElement(s_ccKeypadFacility,*dtmf));
	    Message* msg = message("chan.dtmf");
	    msg->addParam("text",*dtmf);
	    msg->addParam("detected","gsm");
	    dtmfEnqueue(msg);
	}
    }
    else if (*type == s_ccStopDTMF) {
	m_dtmf = 0;
	call->sendCC(YSTRING("StopDTMFAck"));
    }
    else if (*type == s_ccHold) {
	if (!m_mpty)
	    call->sendCC(YSTRING("HoldReject"),buildCCCause("service-unavailable"));
	else {
	    // TODO
	}
    }
    else if (*type == s_ccRetrieve) {
	if (!m_mpty)
	    call->sendCC(YSTRING("RetrieveReject"),buildCCCause("service-unavailable"));
	else {
	    // TODO
	}
    }
    else if (*type == s_ccProceeding || *type == s_ccProgress)
	call->sendWrongState();
    else
	call->sendStatus("unknown-message");
    return true;
}

// Handle media start/alloc response
void YBTSChan::handleMediaStartRsp(bool ok)
{
    if (!ok) {
	Debug(this,DebugNote,"Got media error notification [%p]",this);
	m_waitForTraffic = false;
	hangup("nomedia");
	return;
    }
    Debug(this,DebugAll,"Got media started notification [%p]",this);
    if (isIncoming()) {
	m_waitForTraffic = false;
	if (m_route && !route())
	    deref();
	return;
    }
    if (m_waitForTraffic) {
	Lock lck(m_mutex);
	bool start = (m_pending != 0);
	m_waitForTraffic = false;
	lck.drop();
	if (start)
	    startMT();
    }
    if (__plugin.media()) {
	if (!getSource())
	    __plugin.media()->setSource(this);
	if (!getConsumer())
	    __plugin.media()->setConsumer(this);
    }
}

// Connection released notification
void YBTSChan::connReleased()
{
    Debug(this,DebugInfo,"Connection released [%p]",this);
    Lock lck(m_mutex);
    m_activeCall = 0;
    m_calls.clear();
    setReason("net-out-of-order");
    lck.drop();
    hangup();
}

// Check if MT call can be accepted
bool YBTSChan::canAcceptMT()
{
    if (m_pending || !(m_mpty && m_conn))
	return false;
    Lock lck(m_mutex);
    for (ObjList* l = m_calls.skipNull(); l; l = l->skipNext())
	if (!static_cast<YBTSCallDesc*>(l->get())->active())
	    return false;
    return true;
}

// Start a pending MT call
void YBTSChan::startMT(YBTSConn* conn, bool pagingRsp)
{
    Lock lck(m_mutex);
    if (pagingRsp)
	m_paging = false;
    if (conn && !m_conn) {
	m_conn = conn;
	m_connId = conn->connId();
	__plugin.signalling()->setConnUsage(m_connId,true);
    }
    else
	conn = m_conn;
    if (!conn) {
	if (pagingRsp || !m_paging) {
	    lck.drop();
	    hangup("failure");
	}
	return;
    }
    if (m_cref >= 7 || !m_pending) {
	if (m_cref >= 7)
	    Debug(this,DebugWarn,"Could not allocate new call ref [%p]",this);
	if ((pagingRsp || !m_paging) && !m_calls.skipNull()) {
	    lck.drop();
	    hangup("failure");
	}
	return;
    }
    m_waitForTraffic = !conn->startTraffic();
    if (m_waitForTraffic)
	return;
    String cref((uint16_t)m_cref++);
    YBTSCallDesc* call = new YBTSCallDesc(this,*m_pending,cref);
    m_calls.append(call);
    call->setup(*m_pending);
    TelEngine::destruct(m_pending);
}

void YBTSChan::allocTraffic()
{
    if (m_conn) {
	YBTSMessage m(SigAllocMedia,0,m_conn->connId());
	__plugin.signalling()->send(m);
    }
}

void YBTSChan::stopPaging()
{
    if (!m_paging)
	return;
    Lock lck(m_mutex);
    if (!m_paging)
	return;
    m_paging = false;
    RefPointer<YBTSUE> u = ue();
    lck.drop();
    if (u)
	u->stopPaging();
    u = 0;
}

YBTSCallDesc* YBTSChan::handleSetup(const XmlElement& xml, bool regular, const String* callRef)
{
    const String* type = xml.getAttribute(s_type);
    Lock lck(m_mutex);
    YBTSCallDesc* call = new YBTSCallDesc(this,xml,regular,callRef);
    if (call->null()) {
	TelEngine::destruct(call);
	Debug(this,DebugNote,"%s with empty call ref [%p]",(type ? type->c_str() : "unknown"),this);
	return 0;
    }
    allocTraffic();
    if (!m_calls.skipNull()) {
	call->proceeding();
	m_calls.append(call);
	if (!m_activeCall)
	    m_activeCall = call;
	Debug(this,DebugInfo,"Added call '%s' [%p]",call->c_str(),this);
	m_waitForTraffic = m_conn && m_conn->startTraffic();
	return call;
    }
    bool rlc = !m_waitForTraffic;
    lck.drop();
    Debug(this,DebugNote,"Refusing subsequent call '%s' [%p]",call->c_str(),this);
    if (rlc)
	call->releaseComplete();
    TelEngine::destruct(call);
    return 0;
}

void YBTSChan::hangup(const char* reason, bool final)
{
    releaseRoute();
    ObjList calls;
    Lock lck(m_mutex);
    setReason(reason);
    m_activeCall = 0;
    while (GenObject* o = m_calls.remove(false))
	calls.append(o);
    bool done = m_hungup;
    m_hungup = true;
    String res = m_reason;
    lck.drop();
    for (ObjList* o = calls.skipNull(); o; o = o->skipNull()) {
	YBTSCallDesc* call = static_cast<YBTSCallDesc*>(o->remove(false));
	call->m_reason = res;
	if (call->m_state == YBTSCallDesc::Active) {
	    call->hangup();
	    call->setTimeout(s_t305);
	}
	else {
	    call->release();
	    call->setTimeout(s_t308);
	}
	__plugin.addTerminatedCall(call);
    }
    if (done)
	return;
    Debug(this,DebugCall,"Hangup reason='%s' [%p]",res.c_str(),this);
    Message* m = message("chan.hangup");
    if (res)
	m->setParam("reason",res);
    Engine::enqueue(m);
    if (final)
	return;
    paramMutex().lock();
    NamedList tmp(parameters());
    paramMutex().unlock();
    disconnect(res,tmp);
}

// Release route message.
// Deref if we have a pending message and not final and not connected
bool YBTSChan::releaseRoute(bool final)
{
    Message* m = takeRoute();
    if (!m)
	return false;
    if (!(final || getPeer()))
	deref();
    TelEngine::destruct(m);
    return true;
}

void YBTSChan::disconnected(bool final, const char *reason)
{
    Debug(this,DebugAll,"Disconnected '%s' [%p]",reason,this);
    setReason(reason,&m_mutex);
    Channel::disconnected(final,reason);
}

bool YBTSChan::callRouted(Message& msg)
{
    if (!Channel::callRouted(msg))
	return false;
    Lock lck(m_mutex);
    if (!m_conn)
	return false;
    return true;
}

void YBTSChan::callAccept(Message& msg)
{
    Channel::callAccept(msg);
    if (__plugin.media()) {
	__plugin.media()->setSource(this);
	__plugin.media()->setConsumer(this);
    }
}

void YBTSChan::callRejected(const char* error, const char* reason, const Message* msg)
{
    Channel::callRejected(error,reason,msg);
    setReason(error,&m_mutex);
}

bool YBTSChan::msgProgress(Message& msg)
{
    Channel::msgProgress(msg);
    Lock lck(m_mutex);
    if (m_hungup)
	return false;
    YBTSCallDesc* call = activeCall();
    if (!call)
	return false;
    if (call->incoming())
	call->progressing(buildProgressIndicator(msg));
    return true;
}

bool YBTSChan::msgRinging(Message& msg)
{
    Channel::msgRinging(msg);
    Lock lck(m_mutex);
    if (m_hungup)
	return false;
    YBTSCallDesc* call = activeCall();
    if (!call)
	return false;
    if (call->incoming() && call->m_state == YBTSCallDesc::CallProceeding)
	call->alert(buildProgressIndicator(msg));
    return true;
}

bool YBTSChan::msgAnswered(Message& msg)
{
    Channel::msgAnswered(msg);
    Lock lck(m_mutex);
    if (m_hungup)
	return false;
    YBTSCallDesc* call = activeCall();
    if (!call)
	return false;
    call->connect(buildProgressIndicator(msg,false));
    call->setTimeout(s_t313);
    setTout(call->m_timeout);
    return true;
}

void YBTSChan::checkTimers(Message& msg, const Time& tmr)
{
    if (!m_haveTout)
	return;
    Lock lck(m_mutex);
    if (!m_tout) {
	m_haveTout = false;
	return;
    }
    if (m_tout >= tmr)
	return;
    m_tout = 0;
    m_haveTout = false;
    YBTSCallDesc* call = firstCall();
    if (call) {
	 if (call->m_state == YBTSCallDesc::ConnectReq) {
	    if (call->m_timeout <= tmr) {
		Debug(this,DebugNote,"Call '%s' expired in state %s [%p]",
		    call->c_str(),call->stateName(),this);
		call->m_reason = "timeout";
		call->releaseComplete();
		if (m_activeCall == call)
		    m_activeCall = 0;
		m_calls.remove(call);
		if (!m_calls.skipNull()) {
		    lck.drop();
		    hangup("timeout");
		    return;
		}
	    }
	}
	setTout(call->m_timeout);
    }
}

bool YBTSChan::msgDrop(Message& msg, const char* reason)
{
    releaseRoute();
    setReason(TelEngine::null(reason) ? "dropped" : reason,&m_mutex);
    return Channel::msgDrop(msg,reason);
}

void YBTSChan::destroyed()
{
    hangup(0,true);
    stopPaging();
    TelEngine::destruct(m_pending);
    releaseRoute(true);
    if (m_conn && __plugin.signalling())
	__plugin.signalling()->setConnUsage(m_conn->connId(),false);
    Debug(this,DebugCall,"Destroyed [%p]",this);
    Channel::destroyed();
}


//
// YBTSDriver
//
YBTSDriver::YBTSDriver()
    : Driver("ybts","varchans"),
    m_state(Idle),
    m_stateMutex(false,"YBTSState"),
    m_peerPid(0),
    m_peerAlive(false),
    m_peerCheckTime(0),
    m_peerCheckIntervalMs(YBTS_PEERCHECK_DEF),
    m_error(false),
    m_stopped(false),
    m_stop(false),
    m_stopTime(0),
    m_restart(false),
    m_restartTime(0),
    m_restartIndex(0),
    m_logTrans(0),
    m_logBts(0),
    m_command(0),
    m_media(0),
    m_signalling(0),
    m_mm(0),
    m_haveCalls(false),
    m_engineStart(false),
    m_engineStop(0),
    m_helpCache(0),
    m_smsIndex(0),
    m_ssIndex(0),
    m_mtSmsMutex(false,"YBTSMtSms"),
    m_mtSsMutex(false,"YBTSMtSS"),
    m_mtSsNotEmpty(false),
    m_exportXml(1)
{
    Output("Loaded module YBTS");
    m_ssIdPrefix << prefix() << "ss/";
}

YBTSDriver::~YBTSDriver()
{
    Output("Unloading module YBTS");
    TelEngine::destruct(m_logTrans);
    TelEngine::destruct(m_logBts);
    TelEngine::destruct(m_command);
    TelEngine::destruct(m_media);
    TelEngine::destruct(m_signalling);
    TelEngine::destruct(m_mm);
    TelEngine::destruct(m_helpCache);
}

XmlElement* YBTSDriver::buildCC(XmlElement*& ch, const char* type, const char* callRef, bool tiFlag)
{
    XmlElement* mm = buildCC();
    mm->addChildSafe(buildTID(callRef,tiFlag));
    ch = static_cast<XmlElement*>(mm->addChildSafe(new XmlElement(s_message)));
    if (ch)
	ch->setAttribute(s_type,type);
    return mm;
}

// Export an xml to a list parameter
void YBTSDriver::exportXml(NamedList& list, XmlElement*& xml, const char* param)
{
    if (!xml)
	return;
    String value;
    if (m_exportXml <= 0)
	xml->toString(value);
    if (m_exportXml >= 0) {
	list.addParam(new NamedPointer(param,xml,value));
	xml = 0;
    }
    else {
	list.addParam(param,value);
	TelEngine::destruct(xml);
    }
}

// Handle call control messages
void YBTSDriver::handleCC(YBTSMessage& m, YBTSConn* conn)
{
    XmlElement* xml = m.xml() ? m.xml()->findFirstChild(&s_message) : 0;
    if (!xml) {
	Debug(this,DebugNote,"Empty xml in %s [%p]",m.name(),this);
	return;
    }
    const String* type = xml->getAttribute(s_type);
    if (!type) {
	Debug(this,DebugWarn,"Missing 'type' in %s [%p]",m.name(),this);
	return;
    }
    const String* callRef = 0;
    bool tiFlag = false;
    getTID(*m.xml(),callRef,tiFlag);
    if (conn) {
	RefPointer<YBTSChan> chan;
	findChan(conn->connId(),chan);
	if (chan && chan->handleCC(*xml,callRef,tiFlag))
	    return;
    }
    String cref;
    if (callRef)
	cref << YBTSCallDesc::prefix(tiFlag) << *callRef;
    else
	cref = "with no reference";
    DDebug(this,DebugAll,"Handling '%s' in call %s conn=%u [%p]",
	type->c_str(),cref.c_str(),m.connId(),this);
    bool regular = (*type == s_ccSetup);
    bool emergency = !regular && (*type == s_ccEmergency);
    if (regular || emergency) {
	if (!conn)
	    Debug(this,DebugNote,"Refusing new GSM call, no connection");
	else if (!conn->ue())
	    Debug(this,DebugNote,
		"Refusing new GSM call, no user associated with connection %u",
		conn->connId());
	else if (tiFlag)
	    Debug(this,DebugNote,"Refusing new GSM call, invalid direction");
	else {
	    if (canAccept()) {
		YBTSChan* chan = new YBTSChan(conn);
		if (!chan->initIncoming(*xml,regular,callRef))
		    TelEngine::destruct(chan);
		return;
	    }
	    Debug(this,DebugNote,"Refusing new GSM call, full or exiting");
	}
	if (!TelEngine::null(callRef))
	    YBTSCallDesc::sendGSMRel(false,*callRef,!tiFlag,"noconn",m.connId());
	return;
    }
    if (TelEngine::null(callRef))
	return;
    bool rlc = (*type == s_rlc);
    Lock lck(this);
    // Handle pending calls
    for (ObjList* o = m_terminatedCalls.skipNull(); o; o = o->skipNext()) {
	YBTSCallDesc* call = static_cast<YBTSCallDesc*>(o->get());
	if (cref != *call || call->connId() != m.connId())
	    continue;
	if (rlc || *type == s_ccRel || *type == s_ccDisc) {
	    Debug(this,DebugNote,"Removing terminated call '%s' conn=%u",
		call->c_str(),call->connId());
	    if (!rlc) {
		if (*type == s_ccDisc &&
		    call->m_state == YBTSCallDesc::Disconnect) {
		    call->release();
		    call->setTimeout(s_t308);
		    return;
		}
		call->releaseComplete();
	    }
	    o->remove();
	    m_haveCalls = (0 != m_terminatedCalls.skipNull());
	}
	else if (*type == s_ccStatusEnq)
	    call->sendStatus("status-enquiry-rsp");
	else if (*type != s_ccStatus)
	    call->sendWrongState();
	return;
    }
    if (rlc)
	return;
    lck.drop();
    DDebug(this,DebugInfo,"Unhandled CC %s for callref=%s conn=%p",
	type->c_str(),TelEngine::c_safe(callRef),conn);
    YBTSCallDesc::sendGSMRel(false,*callRef,!tiFlag,"invalid-callref",m.connId());
}

// Handle SMS PDUs
void YBTSDriver::handleSmsPDU(YBTSMessage& m, YBTSConn* conn)
{
    XmlElement* xml = m.xml() ? m.xml()->findFirstChild(&s_message) : 0;
    if (!xml) {
	Debug(this,DebugNote,"Empty xml in %s [%p]",m.name(),this);
	return;
    }
    const String* type = xml->getAttribute(s_type);
    if (!type) {
	Debug(this,DebugWarn,"Missing 'type' in %s [%p]",m.name(),this);
	return;
    }
    const String* callRef = 0;
    bool tiFlag = false;
    if (!getTID(*m.xml(),callRef,tiFlag)) {
	Debug(this,DebugNote,"SMS %s conn=(%p,%u) with missing transaction identifier",
	    type->c_str(),conn,conn ? conn->connId() : 0);
	return;
    }
    if (*type == YSTRING("CP-Data"))
	handleSmsCPData(m,conn,*callRef,tiFlag,*xml);
    else if (*type == YSTRING("CP-Ack"))
	handleSmsCPRsp(m,conn,*callRef,tiFlag,*xml,true);
    else if (*type == YSTRING("CP-Error"))
	handleSmsCPRsp(m,conn,*callRef,tiFlag,*xml,false);
    else
	Debug(this,DebugNote,"Unhandled SMS %s conn=(%p,%u)",
	    type->c_str(),conn,conn ? conn->connId() : 0);
}

// Handle SS PDUs
void YBTSDriver::handleSSPDU(YBTSMessage& m, YBTSConn* conn)
{
    XmlElement* xml = m.xml() ? m.xml()->findFirstChild(&s_message) : 0;
    if (!xml) {
	Debug(this,DebugNote,"Empty xml in %s [%p]",m.name(),this);
	return;
    }
    const String* type = xml->getAttribute(s_type);
    if (!type) {
	Debug(this,DebugWarn,"Missing 'type' in %s [%p]",m.name(),this);
	return;
    }
    const String* callRef = 0;
    bool tiFlag = false;
    if (!getTID(*m.xml(),callRef,tiFlag)) {
	Debug(this,DebugNote,"SS %s conn=(%p,%u) with missing transaction identifier",
	    type->c_str(),conn,conn ? conn->connId() : 0);
	return;
    }
    if (!conn)
	return;
    if (*type == s_facility)
	handleSS(m,conn,*callRef,tiFlag,*xml,true);
    else if (*type == YSTRING("Register"))
	handleSSRegister(m,conn,*callRef,tiFlag,*xml);
    else if (*type == s_rlc)
	handleSS(m,conn,*callRef,tiFlag,*xml,false);
    else
	Debug(this,DebugNote,"Unhandled SS %s conn=(%p,%u)",
	    type->c_str(),conn,conn ? conn->connId() : 0);
}

// Check and start pending MT services for new connection
void YBTSDriver::checkMtService(YBTSUE* ue, YBTSConn* conn, bool pagingRsp)
{
    if (!ue || !conn)
	return;
    RefPointer<YBTSChan> chan;
    if (findChan(ue,chan)) {
	chan->startMT(conn,pagingRsp);
	chan = 0;
    }
    // Check pending SMS
    checkMtSms(ue);
    // Check pending MT SS
    checkMtSs(conn);
}

// Handle media start/alloc response
void YBTSDriver::handleMediaStartRsp(YBTSConn* conn, bool ok)
{
    if (!conn)
	return;
    RefPointer<YBTSChan> chan;
    if (findChan(conn->connId(),chan)) {
	chan->handleMediaStartRsp(ok);
	chan = 0;
    }
    checkMtSms(conn->ue());
    checkMtSs(conn);
}

// Add a pending (wait termination) call
// Send release if it must
void YBTSDriver::addTerminatedCall(YBTSCallDesc* call)
{
    if (!call)
	return;
    call->m_chan = 0;
    if (!call->m_timeout) {
	Debug(this,DebugNote,"Setting terminated call '%s' conn=%u timeout to %ums",
	    call->c_str(),call->connId(),s_t305);
	call->setTimeout(s_t305);
    }
    call->m_timeout = Time::now() + 1000000;
    Lock lck(this);
    m_terminatedCalls.append(call);
    m_haveCalls = true;
}

// Check terminated calls timeout
void YBTSDriver::checkTerminatedCalls(const Time& time)
{
    Lock lck(this);
    for (ObjList* o = m_terminatedCalls.skipNull(); o;) {
	YBTSCallDesc* call = static_cast<YBTSCallDesc*>(o->get());
	if (call->m_timeout > time) {
	    o = o->skipNext();
	    continue;
	}
	// Disconnect: send release, start T308
	// Release: check for resend, restart T308
	if (call->m_state == YBTSCallDesc::Disconnect ||
	    (call->m_state == YBTSCallDesc::Release && call->m_relSent == 1)) {
	    call->release();
	    call->setTimeout(s_t308);
	    o = o->skipNext();
	    continue;
	}
	Debug(this,DebugNote,"Terminated call '%s' conn=%u timed out",
	    call->c_str(),call->connId());
	o->remove();
	o = o->skipNull();
    }
    m_haveCalls = (0 != m_terminatedCalls.skipNull());
}

// Clear terminated calls for a given connection
void YBTSDriver::removeTerminatedCall(uint16_t connId)
{
    Lock lck(this);
    for (ObjList* o = m_terminatedCalls.skipNull(); o;) {
	YBTSCallDesc* call = static_cast<YBTSCallDesc*>(o->get());
	if (call->connId() != connId) {
	    o = o->skipNext();
	    continue;
	}
	Debug(this,DebugInfo,"Removing terminated call '%s' conn=%u: connection released",
	    call->c_str(),call->connId());
	o->remove();
	o = o->skipNull();
    }
    m_haveCalls = (0 != m_terminatedCalls.skipNull());
}

// Handshake done notification. Return false if state is invalid
bool YBTSDriver::handshakeDone()
{
    Lock lck(m_stateMutex);
    if (state() != WaitHandshake) {
	if (state() != Idle) {
	    Debug(this,DebugNote,"Handshake done in %s state !!!",stateName());
	    lck.drop();
	    restart();
	}
	return false;
    }
    changeState(Running);
    return true;
}

void YBTSDriver::connReleased(uint16_t connId)
{
    RefPointer<YBTSChan> chan;
    findChan(connId,chan);
    if (chan) {
	chan->connReleased();
	chan = 0;
    }
    removeTerminatedCall(connId);
    // Reset connection for pending MT SMS
    RefPointer<YBTSMtSmsList> list;
    if (findMtSmsList(list,connId)) {
	list->lock();
	list->m_conn = 0;
	for (ObjList* o = list->m_sms.skipNull(); o; o = o->skipNext()) {
	    YBTSMtSms* sms = static_cast<YBTSMtSms*>(o->get());
	    // Reset sent flag to allow it to be re-sent
	    Lock lck(sms);
	    if (sms->active())
		sms->m_sent = false;
	}
	list->stopPaging();
	list->m_check = true;
	list->unlock();
	list = 0;
    }
}

// Handle USSD update/finalize messages
bool YBTSDriver::handleUssd(Message& msg, bool update)
{
    RefPointer<YBTSConn> conn;
    const String& ssId = msg[YSTRING("peerid")];
    if (!ssId.startsWith(ssIdPrefix()))
	return false;
    int pos = ssId.find("/",ssIdPrefix().length() + 1);
    if (pos > 0)
	// Connection known
	signalling()->findConn(conn,ssId.substr(pos + 1).toInteger(),false);
    else {
	// Check pending SS
	Lock lck(m_mtSsMutex);
	ObjList* o = m_mtSs.find(ssId);
	if (o) {
	    YBTSTid* ss = static_cast<YBTSTid*>(o->get());
	    if (ss->m_type != YBTSTid::Ussd) {
		Debug(this,DebugGoOn,"%s for non USSD session '%s'",
		    msg.c_str(),ssId.c_str());
		msg.setParam(s_error,"failure");
		return false;
	    }
	    if (update) {
		Debug(this,DebugNote,"USSD update for idle session '%s'",ssId.c_str());
		msg.setParam(s_error,"failure");
		return false;
	    }
	    o->remove(false);
	    lck.drop();
	    Debug(this,DebugInfo,"MT USSD '%s' cancelled",ss->c_str());
	    TelEngine::destruct(ss);
	    return true;
	}
	// Find a connection owning the session
	signalling()->findConnSSTid(conn,ssId);
    }
    Lock lck(conn);
    YBTSTid* ss = conn ? conn->getSSTid(ssId,!update) : 0;
    if (!ss) {
	Debug(this,DebugNote,"USSD %s: no session with id '%s'",
	    update ? "update" : "finalize",ssId.c_str());
	msg.setParam(s_error,"invalid-callref");
	return false;
    }
    String facility;
    String reason;
    getUssdFacility(msg,facility,reason,ss,update);
    const char* cause = 0;
    bool ok = true;
    const char* error = "interworking";
    if (update) {
	ok = !facility.null();
	if (!ok) {
	    Debug(this,DebugNote,"USSD update session '%s': %s",
		ssId.c_str(),reason.safe("no facility to send on session"));
	    error = "failure";
	}
    }
    else {
	cause = msg.getValue(s_error);
	if (reason)
	    Debug(this,DebugNote,"USSD finalize session '%s' not sending facility: %s",
		ssId.c_str(),reason.c_str());
    }
    ok = ok && signalling()->sendSS(update,conn,ss->m_callRef,ss->m_incoming,ss->m_sapi,
	cause,facility);
    lck.drop();
    if (!update)
	TelEngine::destruct(ss);
    if (ok)
	msg.clearParam(s_error);
    else
	msg.setParam(s_error,error);
    return ok;
}

// Handle USSD execute messages
bool YBTSDriver::handleUssdExecute(Message& msg, String& dest)
{
    if ((m_state != RadioUp) || !(m_signalling && m_mm)) {
	Debug(this,DebugWarn,"MT USSD to '%s': Radio is not up!",dest.c_str());
	msg.setParam(s_error,"interworking");
	return false;
    }
    RefPointer<YBTSUE> ue;
    if (!m_mm->findUESafe(ue,dest)) {
	// We may consider paging UEs that are not in the TMSI table
	Debug(this,DebugNote,"MT USSD to '%s': offline",dest.c_str());
	msg.setParam(s_error,"offline");
	return false;
    }
    String facility;
    String reason;
    if (!getUssdFacility(msg,facility,reason)) {
	Debug(this,DebugNote,"MT USSD to '%s' failed: %s",
	    dest.c_str(),reason.safe("empty text"));
	msg.setParam(s_error,"failure");
	return false;
    }
    String ssId;
    ssId << ssIdPrefix() << ssIndex();
    YBTSTid* ss = new YBTSTid(YBTSTid::Ussd,"0",false,0,ssId);
    unsigned int tout = msg.getIntValue(YSTRING("timeout"),s_ussdTimeout,
	YBTS_USSD_TIMEOUT_MIN);
    ss->m_timeout = Time::now() + (uint64_t)tout * 1000;
    ss->m_data = facility;
    ss->m_peerId = msg[YSTRING("id")];
    ss->m_startCID = "0";
    ss->m_ue = ue;
    ss->m_cid = 0;
    RefPointer<YBTSConn> conn;
    if (!m_signalling->findConn(conn,ue) || conn->waitForTraffic()) {
	if (!conn) {
	    ss->m_paging = ue->startPaging(ChanTypeSS);
	    if (!ss->m_paging) {
		TelEngine::destruct(ss);
		Debug(this,DebugNote,"MT USSD to '%s' failed to start paging",dest.c_str());
		msg.setParam(s_error,"failure");
		return false;
	    }
	}
	m_mtSsMutex.lock();
	m_mtSs.append(ss);
	m_mtSsNotEmpty = true;
	String tmp;
	Debug(this,DebugInfo,"Enqueued MT USSD session '%s' to IMSI='%s'",
	    ss->c_str(),ue->imsiSafe(tmp).c_str());
	m_mtSsMutex.unlock();
    }
    else {
	const char* fail = startMtSs(conn,ss);
	TelEngine::destruct(ss);
	if (fail) {
	    msg.setParam(s_error,fail);
	    return false;
	}
    }
    msg.setParam("peerid",ssId);
    return true;
}

bool YBTSDriver::getUssdFacility(NamedList& list, String& buf, String& reason,
    YBTSTid* ss, bool update)
{
    buf = list[YSTRING("component")];
    if (buf)
	return true;
    const String& text = list[YSTRING("text")];
    if (!text)
	return false;
    bool invoke = true;
    const String& oper = list[YSTRING("operation_type")];
    int op = ussdOper(oper);
    // SS given: not a an MT USSD
    if (ss) {
	if (op == Ussr || op == Ussn) {
	    if (!update)
		op = USSDMapOperUnknown;
	}
	else if (op == Pssr) {
	    if (ss->m_incoming)
		invoke = false;
	    else
		op = USSDMapOperUnknown;
	}
	else
	    op = USSDMapOperUnknown;
    }
    else if (op != Ussr && op != Ussn)
	op = USSDMapOperUnknown;
    if (op == USSDMapOperUnknown) {
	reason << "unknown operation '" << oper << "'";
	return false;
    }
    XmlElement* x = new XmlElement(s_mapComp);
    if (invoke)
	x->setAttribute(s_mapLocalCID,ss ? String(ss->nextCID()).c_str() : "0");
    else
	x->setAttributeValid(s_mapRemoteCID,ss ? ss->m_startCID.c_str() : "0");
    x->setAttribute(s_mapCompType,invoke ? "Invoke" : "ResultLast");
    x->setAttribute(s_mapOperCode,ussdMapOperName(op));
    XmlElement* txt = new XmlElement(s_mapUssdText,text);
    txt->setAttributes(list,"text.");
    x->addChildSafe(txt);
    String error;
    mapEncodeComp(x,buf,&error);
    if (buf)
	return true;
    reason << "encode failed";
    reason.append(error," error=");
    return false;
}

// Start an MT USSD
const char* YBTSDriver::startMtSs(YBTSConn* conn, YBTSTid*& ss)
{
    if (!(conn && ss))
	return "failure";
    String imsi;
    if (conn->ue())
	conn->ue()->imsiSafe(imsi);
    ss->setConnId(conn->connId());
    Lock lck(conn);
    if (conn->m_ss) {
	lck.drop();
	Debug(this,DebugNote,"MT USSD '%s' IMSI='%s' failed: SS busy",
	    ss->c_str(),imsi.c_str());
	return "busy";
    }
    conn->m_ss = ss;
    uint64_t tout = ss->m_timeout;
    String ssId = *ss;
    String callRef = ss->m_callRef;
    uint8_t sapi = ss->m_sapi;
    String facility = ss->m_data;
    ss = 0;
    lck.drop();
    m_signalling->setConnToutCheck(tout);
    if (m_signalling->sendSSRegister(conn,callRef,sapi,facility)) {
	Debug(this,DebugInfo,"MT USSD '%s' IMSI='%s' started",
	    ssId.c_str(),imsi.c_str());
	return 0;
    }
    // Take it back
    ss = conn->takeSSTid(ssId);
    Debug(this,DebugNote,"MT USSD '%s' IMSI='%s' failed to start",
	ssId.c_str(),imsi.c_str());
    return "failure";
}

bool YBTSDriver::radioReady()
{
    Lock lck(m_stateMutex);
    if (state() != Running) {
	Debug(this,DebugNote,"Radio Ready in %s state !!!",stateName());
	lck.drop();
	restart();
	return false;
    }
    m_restartIndex = 0;
    changeState(RadioUp);
    return true;
}

void YBTSDriver::restart(unsigned int restartMs, unsigned int stopIntervalMs)
{
    Lock lck(m_stateMutex);
    if (m_error)
	return;
    if (m_restartTime && !restartMs)
	m_restart = true;
    else
	setRestart(1,true,restartMs);
    if (state() != Idle)
	setStop(stopIntervalMs);
}

void YBTSDriver::stopNoRestart()
{
    Lock lck(m_stateMutex);
    setStop(0);
    m_restart = false;
    m_restartTime = 0;
    m_error = true;
}

// Enqueue an SS related message. Decode facility
// Don't enqueue facility messages if decode fails
bool YBTSDriver::enqueueSS(Message* m, const String* facilityIE, bool facility,
    const char* error)
{
    if (!m)
	return false;
    if (!facility)
	m->addParam(s_error,error);
    XmlElement* xml = 0;
    String e;
    if (!TelEngine::null(facilityIE))
	xml = mapDecode(*facilityIE,&e);
    if (xml) {
	XmlElement* comp = xml->findFirstChild(&s_mapComp);
	const String* oper = comp ? comp->getAttribute(s_mapOperCode) : 0;
	if (!TelEngine::null(oper)) {
	    int op = ussdMapOper(*oper);
	    m->addParam("operation_type",ussdOperName(op),false);
	}
	XmlElement* textXml = comp->findFirstChild(&s_mapUssdText);
	const String& text = textXml ? textXml->getText() : String::empty();
	if (text) {
	    m->addParam("text",text);
	    textXml->copyAttributes(*m,"text.");
	}
	exportXml(*m,xml);
    }
    else if (facility) {
	Debug(this,DebugNote,
	    "Can't enqueue '%s': failed to decode facility '%s' error='%s'",
	    m->c_str(),TelEngine::c_safe(facilityIE),e.safe());
	TelEngine::destruct(m);
	return false;
    }
    else if (!TelEngine::null(facilityIE))
	Debug(this,DebugNote,
	    "Enqueueing '%s', failed to decode facility '%s' error='%s'",
	    m->c_str(),facilityIE->c_str(),e.safe());
    return Engine::enqueue(m);
}

// Decode MAP content
XmlElement* YBTSDriver::mapDecode(const String& buf, String* error, bool decodeUssd)
{
    if (!buf)
	return 0;
    Message m("map.decode");
    m.addParam("module",name());
    m.addParam("data",buf);
    m.addParam("xmlstr",String::boolText(m_exportXml <= 0));
    if (decodeUssd)
	m.addParam("ussd-handle",String::boolText(true));
    bool ok = Engine::dispatch(m);
    const char* e = m.getValue(s_error);
    if (error)
	*error = e;
    NamedPointer* np = YOBJECT(NamedPointer,m.getParam(YSTRING("xml")));
    XmlElement* xml = YOBJECT(XmlElement,np);
    if (xml && xml->findFirstChild())
	np->takeData();
    else
	xml = 0;
#ifdef XDEBUG
    String s;
    if (xml) {
	xml->toString(s,false,"\r\n","  ");
	s = "\r\n-----" + s + "\r\n-----";
    }
    Debug(this,DebugAll,"mapDecode() '%s' ok=%s error='%s'%s",
	buf.c_str(),String::boolText(ok),e,s.safe());
#endif
    if (!ok)
	Debug(this,xml ? DebugInfo : DebugNote,
	    "MAP decode failed error='%s' xml=(%p)",e,xml);
    return xml;
}

// Encode MAP content
bool YBTSDriver::mapEncode(XmlElement*& xml, String& buf, String* error, bool encodeUssd)
{
    if (!xml)
	return false;
    Message m("map.encode");
    m.addParam("module",name());
    if (encodeUssd)
	m.addParam("ussd-handle",String::boolText(true));
    exportXml(m,xml);
    bool ok = Engine::dispatch(m);
    buf = m[YSTRING("data")];
    if (ok && buf)
	return true;
    const char* e = m.getValue(s_error);
    if (error)
	*error = e;
    Debug(this,DebugNote,"MAP encode failed ret=%s error='%s' data: %s",
	String::boolText(ok),e,buf.c_str());
    buf.clear();
    return false;
}

void YBTSDriver::handleSmsCPData(YBTSMessage& m, YBTSConn* conn,
    const String& callRef, bool tiFlag, const XmlElement& cpData)
{
    if (!conn) {
	Debug(this,DebugMild,"Ignoring SMS CP-DATA conn=%u: no connection",m.connId());
	return;
    }
    int level = DebugNote;
    String reason;
    const char* cause = "protocol-error";
    uint8_t causeRp = 111;
    bool cpOk = false;
    // tiFlag=true means this is a response to a transaction started by us
    while (true) {
#define SMS_CPDATA_DONE(str) { \
    if (tiFlag && conn) \
	handleMtSmsRsp(conn->ue(),false,callRef); \
    YBTS_SET_REASON_BREAK(str); \
}
#define SMS_CPDATA_DONE_MILD(str) { level = DebugMild; SMS_CPDATA_DONE(str); }
	if (!(conn && conn->ue()))
	    SMS_CPDATA_DONE("missing UE");
	if (!conn->ue()->registered())
	    SMS_CPDATA_DONE("UE not registered");
	cause = "invalid-mandatory-info";
	const String* rpdu = cpData.childText(YSTRING("RPDU"));
	if (TelEngine::null(rpdu))
	    SMS_CPDATA_DONE("empty RPDU");
	uint8_t rpMsgType = 0;
	uint8_t rpMsgRef = 0;
	String called, plan, type;
	String smsCalled, smsCalledPlan, smsCalledType, smsText, smsTextEnc;
	int res = decodeRP(*rpdu,rpMsgType,rpMsgRef,&called,&plan,&type,
	    &smsCalled,&smsCalledPlan,&smsCalledType,&smsText,&smsTextEnc);
	if (res) {
	    if (res > 0)
		SMS_CPDATA_DONE_MILD("invalid RPDU string");
	    if (res != -2)
		SMS_CPDATA_DONE("invalid RPDU length");
	}
	if (tiFlag) {
	    bool ok = (rpMsgType == RPAckFromMs);
	    if (handleMtSmsRsp(conn->ue(),ok,callRef,0,*rpdu,m.info()))
		return;
	    cause = "message-not-compatible-with-SM-protocol-state";
	    reason = "unexpected RP-DATA";
	    break;
	}
	// CP-DATA is ok, accept it
	cpOk = true;
	signalling()->sendSmsCPRsp(conn,callRef,!tiFlag,m.info());
	// Add pending incoming SMS info
	signalling()->addMOSms(conn,callRef,m.info(),rpMsgRef);
	// Check for RP message
	if (rpMsgType != RPDataFromMs && rpMsgType != RPSMMAFromMs) {
	    if (rpMsgType == RPAckFromMs || rpMsgType == RPErrorFromMs) {
		causeRp = 98; // Unexpected message
		SMS_CPDATA_DONE("unhandled RP-ACK or RP-ERROR");
	    }
	    causeRp = 97; // Unknown message type
	    SMS_CPDATA_DONE("unknown RP message " + String(rpMsgType));
	}
	if (rpMsgType == RPSMMAFromMs) {
	    causeRp = 98; // Unexpected message
	    SMS_CPDATA_DONE("unhandled RP-SMMA");
	}
	// RP-DATA from MS
	if (!called)
	    Debug(this,DebugNote,
		"SMS CP-DATA conn=%u: unable to retrieve SMSC number, %s",
		m.connId(),res ? "empty destination address" : "invalid RP-DATA");
	YBTSSubmit* th = new YBTSSubmit(YBTSTid::Sms,conn->connId(),conn->ue(),callRef);
	if (called) {
	    th->msg().addParam("called",called);
	    th->msg().addParam("callednumplan",plan,false);
	    th->msg().addParam("callednumtype",type,false);
	}
	if (smsCalled) {
	    th->msg().addParam("sms.called",smsCalled);
	    th->msg().addParam("sms.called.plan",smsCalledPlan,false);
	    th->msg().addParam("sms.called.nature",smsCalledType,false);
	}
	if (smsText) {
	    th->msg().addParam("text",smsText);
	    th->msg().addParam("text.encoding",smsTextEnc);
	}
	th->msg().addParam("rpdu",*rpdu);
	if (th->startup())
	    return;
	causeRp = 41; // Temporary failure
	SMS_CPDATA_DONE_MILD("failed to start thread");
#undef SMS_CPDATA_DONE
#undef SMS_CPDATA_DONE_MILD
    }
    if (!cpOk) {
	Debug(this,level,"Rejecting SMS CP-DATA conn=%u: %s",
	    conn->connId(),reason.c_str());
	signalling()->sendSmsCPRsp(conn,callRef,!tiFlag,m.info(),cause);
	return;
    }
    if (tiFlag)
	return;
    Debug(this,level,"Rejecting SMS CP-DATA conn=%u RP-Cause=%u: %s",
	conn->connId(),causeRp,reason.c_str());
    signalling()->moSmsRespond(conn,callRef,causeRp);
}

void YBTSDriver::handleSmsCPRsp(YBTSMessage& m, YBTSConn* conn,
    const String& callRef, bool tiFlag, const XmlElement& rsp, bool ok)
{
    Debug(this,ok ? DebugAll : DebugNote,"SMS %s conn=%u callRef=%s tiFlag=%s",
	(ok ? "CP-ACK" : "CP-ERROR"),m.connId(),callRef.c_str(),
	String::boolText(tiFlag));
    if (!tiFlag) {
	YBTSSmsInfo* info = signalling()->removeSms(conn,callRef,true);
	TelEngine::destruct(info);
	return;
    }
    // Response to MT SMS transaction
    // Ack: Wait for CP-DATA with RP-DATA
    // Error: Stop waiting
    if (!ok && conn)
	handleMtSmsRsp(conn->ue(),false,callRef);
}

bool YBTSDriver::checkMtSms(YBTSMtSmsList& list)
{
    Lock lck(list);
    ObjList* o = list.m_sms.skipNull();
    XDebug(this,DebugAll,"checkMtSms(%p)%s",&list,(o ? "" : " empty"));
    if (!o)
	return false;
    YBTSMtSms* sms = static_cast<YBTSMtSms*>(o->get());
    if (!sms->active()) {
	XDebug(this,DebugAll,"checkMtSms(%p) removing inactive '%s'",
	    &list,sms->id().c_str());
	o->remove();
	lck.drop();
	return checkMtSms(list);
    }
    DDebug(this,DebugAll,
	"checkMtSms(%p) '%s' sent=%u paging=%u conn=%p waitTraffic=%u",
	&list,sms->id().c_str(),sms->sent(),!list.ue()->paging().null(),
	(YBTSConn*)(list.m_conn),(list.m_conn ? list.m_conn->waitForTraffic() : false));
    // Do nothing if radio is not up
    if (m_state != RadioUp)
	return true;
    if (sms->sent())
	return true;
    if (list.paging()) {
	if (list.ue() && list.ue()->paging())
	    return true;
	list.stopPaging();
    }
    if (!list.m_conn) {
	// No connection: start paging
	if (!m_signalling->findConn(list.m_conn,list.ue())) {
	    list.startPaging();
	    return true;
	}
	list.stopPaging();
	signalling()->setConnUsage(list.m_conn->connId(),true,true);
    }
    // Wait for traffic to start (channel mode change was requested)
    if (list.m_conn->waitForTraffic())
	return true;
    // Check for SAPI 3 availability
    uint8_t sapi = list.m_conn->startSapi(3);
    if (sapi == 255)
	return true;
    sms->m_callRef = list.m_tid++;
    if (list.m_tid >= 7)
	list.m_tid = 0;
    if (signalling()->sendSmsCPData(list.m_conn,sms->callRef(),false,sapi,sms->rpdu())) {
	Debug(this,DebugAll,"MT SMS '%s' to IMSI=%s sent on conn %u",
	    sms->id().c_str(),list.ue()->imsi().c_str(),list.m_conn->connId());
	sms->m_sent = true;
	return true;
    }
    // Failed to send: terminate now
    sms->terminate(false);
    o->remove();
    lck.drop();
    return checkMtSms(list);
}

void YBTSDriver::checkMtSs(YBTSConn* conn)
{
    if (!(conn && conn->ue()))
	return;
    if (conn->waitForTraffic())
	return;
    ObjList list;
    m_mtSsMutex.lock();
    for (ObjList* o = m_mtSs.skipNull(); o;) {
	YBTSTid* ss = static_cast<YBTSTid*>(o->get());
	if (conn->ue() == ss->m_ue) {
	    list.append(o->remove(false));
	    o = o->skipNull();
	}
	else
	    o = o->skipNext();
    }
    m_mtSsNotEmpty = (m_mtSs.skipNull() != 0);
    m_mtSsMutex.unlock();
    for (ObjList* o = list.skipNull(); o; o = o->skipNull()) {
	YBTSTid* ss = static_cast<YBTSTid*>(o->remove(false));
	ss->m_ue->stopPaging();
	ss->m_paging = false;
	if (startMtSs(conn,ss))
	    continue;
	if (ss) {
	    enqueueSS(ss->ssMessage(false),0,false,"busy");
	    TelEngine::destruct(ss);
	}
    }
}

// Drop all pending SS
void YBTSDriver::dropAllSS(const char* reason)
{
    ObjList list;
    m_mtSsMutex.lock();
    moveList(list,m_mtSs);
    m_mtSsNotEmpty = false;
    m_mtSsMutex.unlock();
    for (ObjList* o = list.skipNull(); o; o = o->skipNext()) {
	YBTSTid* ss = static_cast<YBTSTid*>(o->remove(false));
	Debug(this,DebugInfo,"Dropping idle MT USSD '%s' reason=%s",ss->c_str(),reason);
	enqueueSS(ss->ssMessage(false),0,false,reason);
    }
}

// Handle pending MP SMS final response
// Return false if not found
bool YBTSDriver::handleMtSmsRsp(YBTSUE* ue, bool ok, const String& callRef,
    const char* reason, const char* rpdu, uint8_t respondSapi)
{
    if (!ue)
	return false;
    RefPointer<YBTSMtSmsList> list;
    if (!findMtSmsList(list,ue))
	return false;
    Lock lck(list);
    ObjList* first = list->m_sms.skipNull();
    if (!first) {
	lck.drop();
	removeMtSms(list);
	return false;
    }
    YBTSMtSms* sms = static_cast<YBTSMtSms*>(first->get());
    if (!sms->active()) {
	first->remove();
	lck.drop();
	return handleMtSmsRsp(ue,ok,callRef,reason,rpdu);
    }
    if (sms->callRef() != callRef)
	return false;
    Debug(this,DebugAll,"MT SMS '%s' to IMSI=%s responded",
	sms->id().c_str(),ue->imsi().c_str());
    sms->terminate(ok,reason,rpdu);
    first->remove();
    lck.drop();
    if (respondSapi != 255)
	signalling()->sendSmsCPRsp(list->m_conn,callRef,false,respondSapi);
    if (!checkMtSms(*list))
	removeMtSms(list);
    return true;
}

YBTSMtSmsList* YBTSDriver::findMtSmsList(YBTSUE* ue, bool create)
{
    if (!ue)
	return 0;
    ObjList* append = &m_mtSms;
    for (ObjList* o = m_mtSms.skipNull(); o; o = o->skipNext()) {
	YBTSMtSmsList* tmp = static_cast<YBTSMtSmsList*>(o->get());
	if (ue == tmp->ue())
	    return tmp;
	append = o;
    }
    YBTSMtSmsList* list = 0;
    if (create) {
	list = new YBTSMtSmsList(ue);
	if (list->ue())
	    append->append(list);
	else
	    TelEngine::destruct(list);
    }
    return list;
}

YBTSMtSmsList* YBTSDriver::findMtSmsList(uint16_t connId)
{
    for (ObjList* o = m_mtSms.skipNull(); o; o = o->skipNext()) {
	YBTSMtSmsList* tmp = static_cast<YBTSMtSmsList*>(o->get());
	if (tmp->m_conn && tmp->m_conn->connId() == connId)
	    return tmp;
    }
    return 0;
}

// Remove a pending sms. Remove its queue if empty
bool YBTSDriver::removeMtSms(YBTSMtSmsList* list, YBTSMtSms* sms)
{
    if (!list)
	return false;
    Lock2 lck(m_mtSmsMutex,*list);
    if (sms)
	list->m_sms.remove(sms);
    bool removed = !list->m_sms.skipNull() && m_mtSms.remove(list,false);
    lck.drop();
    if (removed)
	list->deref();
    return removed;
}

// Handle SS Facility or Release Complete
void YBTSDriver::handleSS(YBTSMessage& m, YBTSConn* conn, const String& callRef,
    bool tiFlag, XmlElement& xml, bool facility)
{
    Lock lck(conn);
    YBTSTid* ss = conn ? conn->getSSTid(callRef,!tiFlag,!facility) : 0;
    if (!ss) {
	Debug(this,DebugNote,"SS %s on conn=%u: unknown session",
	    xml.attribute(s_type),m.connId());
	lck.drop();
	if (facility)
	    signalling()->sendSS(false,conn,callRef,!tiFlag,m.info(),"invalid-callref");
	return;
    }
    Message* msg = ss->ssMessage(facility);
    lck.drop();
    if (!facility)
	TelEngine::destruct(ss);
    enqueueSS(msg,xml.childText(s_facility),facility);
}

void YBTSDriver::handleSSRegister(YBTSMessage& m, YBTSConn* conn, const String& callRef,
    bool tiFlag, XmlElement& xml)
{
    if (conn)
	signalling()->setConnUsage(conn->connId(),true);
    XmlElement* facilityXml = 0;
    String reason;
    YBTSTid* ss = 0;
    const char* cause = "protocol-error";
    while (true) {
	if (tiFlag) {
	    cause = "invalid-callref";
	    YBTS_SET_REASON_BREAK("wrong TI flag 'true'");
	}
	if (!(conn && conn->ue()))
	    YBTS_SET_REASON_BREAK("missing UE");
	if (!conn->ue()->registered())
	    YBTS_SET_REASON_BREAK("UE not registered");
	const String* facility = xml.childText(s_facility);
	if (TelEngine::null(facility)) {
	    cause = "missing-mandatory-ie";
	    YBTS_SET_REASON_BREAK("empty Facility IE");
	}
	String error;
	facilityXml = mapDecode(*facility,&error);
	XmlElement* comp = facilityXml ? facilityXml->findFirstChild(&s_mapComp) : 0;
	if (!comp) {
	    reason << (facilityXml ? "failed to retrieve Facility IE component" :
		"failed to decode Facility IE");
	    reason.append(error," error=");
	    break;
	}
	const String* cid = comp->getAttribute(s_mapRemoteCID);
	if (TelEngine::null(cid))
	    YBTS_SET_REASON_BREAK("missing remote CID");
	const String* compType = comp->getAttribute(s_mapCompType);
	if (TelEngine::null(compType) || *compType != YSTRING("Invoke")) {
	    reason << "unexpected Facility IE component type '" << compType << "'";
	    break;
	}
	YBTSTid::Type t = YBTSTid::Unknown;
	const String* oper = comp->getAttribute(s_mapOperCode);
	if (oper) {
	    if (*oper == ussdMapOperName(Pssr))
		t = YBTSTid::Ussd;
	}
	if (t != YBTSTid::Ussd) {
	    reason << "unknown Facility operation code " << oper;
	    break;
	}
	String ssId;
	ssId << ssIdPrefix() << ssIndex() << "/" << conn->connId();
	ss = new YBTSTid(YBTSTid::Ussd,callRef,true,m.info(),ssId);
	ss->m_startCID = cid;
	ss->setConnId(conn->connId());
	Lock lck(conn);
	if (conn->findSSTid(callRef,!tiFlag)) {
	    Debug(this,DebugNote,"Ignoring SS %s on conn=%u TID=%s: already exists",
		xml.attribute(s_type),m.connId(),callRef.c_str());
	    break;
	}
	if (conn->hasSS())
	    YBTS_SET_REASON_BREAK("SS busy");
	if (t == YBTSTid::Ussd) {
	    conn->m_ss = ss;
	    uint64_t tout = Time::now() + (uint64_t)s_ussdTimeout * 1000;
	    ss->m_timeout = tout;
	    ss = 0;
	    lck.drop();
	    signalling()->setConnToutCheck(tout);
	    submitUssd(conn,callRef,ssId,facilityXml,comp);
	    break;
	}
	reason << "unhandled Facility type " << t;
	break;
    }
    String s;
    if (reason && facilityXml) {
	facilityXml->toString(s);
	s = "(xml: " + s + ")";
    }
    TelEngine::destruct(facilityXml);
    TelEngine::destruct(ss);
    if (reason) {
	Debug(this,DebugNote,"Rejecting SS %s on conn=%u: %s%s",
	    xml.attribute(s_type),m.connId(),reason.c_str(),s.safe());
	signalling()->sendSS(false,conn,callRef,!tiFlag,m.info(),cause);
    }
    if (conn)
	signalling()->setConnUsage(conn->connId(),false);
}

bool YBTSDriver::submitUssd(YBTSConn* conn, const String& callRef, const String& ssId,
    XmlElement*& facilityXml, XmlElement* comp)
{
    if (!(conn && conn->ue()))
	return false;
    const char* reason = 0;
    while (true) {
	XmlElement* textXml = comp ? comp->findFirstChild(&s_mapUssdText) : 0;
	const String& text = textXml ? textXml->getText() : String::empty();
	if (!text) {
	    reason = "empty USSD string";
	    break;
	}
	YBTSSubmit* th = new YBTSSubmit(YBTSTid::Ussd,conn->connId(),conn->ue(),callRef);
	th->msg().addParam("called",text);
	th->msg().addParam("id",ssId);
	th->msg().addParam("operation_type",ussdOperName(Pssr));
	th->msg().addParam("text",text);
	textXml->copyAttributes(th->msg(),"text.");
	exportXml(th->msg(),facilityXml);
	if (th->startup())
	    return true;
	delete th;
	reason = "failed to start routing thread";
	break;
    }
    Debug(this,DebugNote,"Rejecting MO USSD on conn=%u: %s",conn->connId(),reason);
    NamedList p("");
    p.addParam(s_error,"facility-rejected");
    signalling()->moSSExecuted(conn,callRef,false,p);
    return false;
}

void YBTSDriver::checkMtSsTout(const Time& time)
{
    ObjList list;
    m_mtSsMutex.lock();
    for (ObjList* o = m_mtSs.skipNull(); o;) {
	YBTSTid* ss = static_cast<YBTSTid*>(o->get());
	if (ss->m_timeout <= time) {
	    list.append(o->remove(false));
	    o = o->skipNull();
	}
	else
	    o = o->skipNext();
    }
    m_mtSsNotEmpty = (m_mtSs.skipNull() != 0);
    m_mtSsMutex.unlock();
    for (ObjList* o = list.skipNull(); o; o = o->skipNext()) {
	YBTSTid* ss = static_cast<YBTSTid*>(o->get());
	Debug(this,DebugInfo,"Idle MT USSD '%s' timed out",ss->c_str());
	enqueueSS(ss->ssMessage(false),0,false,"timeout");
    }
}

void YBTSDriver::start()
{
    stop();
    Lock lck(m_stateMutex);
    if (m_stopped)
	return;
    unsigned int n = s_restartMax;
    if (m_restartIndex >= n) {
	m_stopped = true;
	Alarm(this,"system",DebugGoOn,
	    "Restart index reached maximum value %u. Exiting ...",n);
	Engine::halt(ECANCELED);
	return;
    }
    m_restartIndex++;
    changeState(Starting);
    while (true) {
	// Log interface
	if (!m_logTrans->start())
	    break;
	if (!m_logBts->start())
	    break;
	// Command interface
	if (!m_command->start())
	    break;
	// Signalling interface
	if (!m_signalling->start())
	    break;
	// Media interface
	if (!m_media->start())
	    break;
	// Start peer application
	if (!startPeer())
	    break;
	changeState(WaitHandshake);
	m_signalling->waitHandshake();
	setRestart(0);
	return;
    }
    setRestart(0,1);
    Alarm(this,"system",DebugWarn,"Failed to start the BTS");
    lck.drop();
    stop();
}

void YBTSDriver::stop()
{
    dropAllSS();
    lock();
    ListIterator iter(channels());
    while (true) {
	GenObject* gen = iter.get();
	if (!gen)
	    break;
	RefPointer<YBTSChan> chan = static_cast<YBTSChan*>(gen);
	if (!chan)
	    continue;
	unlock();
	chan->stop();
	chan = 0;
	lock();
    }
    m_terminatedCalls.clear();
    m_haveCalls = false;
    unlock();
    Lock lck(m_stateMutex);
    bool stopped = (state() != Idle);
    if (stopped)
	Debug(this,DebugAll,"Stopping ...");
    m_stop = false;
    m_stopTime = 0;
    m_error = false;
    m_signalling->stop();
    m_media->stop();
    stopPeer();
    m_command->stop();
    m_logTrans->stop();
    m_logBts->stop();
    m_peerAlive = false;
    m_peerCheckTime = 0;
    changeState(Idle);
    lck.drop();
    channels().clear();
    if (stopped && m_stopped)
	Debug(this,DebugNote,"Stopped, waiting for command to start");
    if (Engine::exiting()) {
	m_restart = false;
	m_restartTime = 0;
	m_error = true;
	m_stopped = true;
    }
}

bool YBTSDriver::startPeer()
{
    String cmd,arg,dir;
    getGlobalStr(cmd,s_peerCmd);
    getGlobalStr(arg,s_peerArg);
    getGlobalStr(dir,s_peerDir);
    if (!cmd) {
	Alarm(this,"config",DebugConf,"Empty peer path");
	return false;
    }
    Debug(this,DebugAll,"Starting peer '%s' '%s'",cmd.c_str(),arg.c_str());
    Socket s[FDCount];
    s[FDLogTransceiver].attach(m_logTrans->transport().detachRemote());
    s[FDLogBts].attach(m_logBts->transport().detachRemote());
    s[FDCommand].attach(m_command->transport().detachRemote());
    s[FDSignalling].attach(m_signalling->transport().detachRemote());
    s[FDMedia].attach(m_media->transport().detachRemote());
    int pid = ::fork();
    if (pid < 0) {
	String s;
	Alarm(this,"system",DebugWarn,"Failed to fork(): %s",addLastError(s,errno).c_str());
	return false;
    }
    if (pid) {
	Debug(this,DebugInfo,"Started peer pid=%d",pid);
	m_peerPid = pid;
	return true;
    }
    // In child - terminate all other threads if needed
    Thread::preExec();
    // Try to immunize child from ^C and ^backslash
    ::signal(SIGINT,SIG_IGN);
    ::signal(SIGQUIT,SIG_IGN);
    // Restore default handlers for other signals
    ::signal(SIGTERM,SIG_DFL);
    ::signal(SIGHUP,SIG_DFL);
    // Attach socket handles
    int i = STDERR_FILENO + 1;
    for (int j = 0; j < FDCount; j++, i++) {
	int h = s[j].handle();
	if (h < 0)
	    ::fprintf(stderr,"Socket handle at index %d (fd=%d) not used, weird...\n",j,i);
	else if (h < i)
	    ::fprintf(stderr,"Oops! Overlapped socket handle old=%d new=%d\n",h,i);
	else if (h == i)
	    continue;
	else if (::dup2(h,i) < 0)
	    ::fprintf(stderr,"Failed to set socket handle at index %d: %d %s\n",
		j,errno,strerror(errno));
	else
	    continue;
	::close(i);
    }
    // Close all other handles
    for (; i < 1024; i++)
	::close(i);
    // Let MBTS know where to pick the configuration file from
    if (s_configFile)
	::setenv("MBTSConfigFile",s_configFile,true);
    // Change directory if asked
    if (dir && ::chdir(dir))
    ::fprintf(stderr,"Failed to change directory to '%s': %d %s\n",
	dir.c_str(),errno,strerror(errno));
    // Start
    ::execl(cmd.c_str(),cmd.c_str(),arg.c_str(),(const char*)0);
    ::fprintf(stderr,"Failed to execute '%s': %d %s\n",
	cmd.c_str(),errno,strerror(errno));
    // Shit happened. Die !!!
    ::_exit(1);
    return false;
}

static int waitPid(pid_t pid, unsigned int interval)
{
    int ret = -1;
    for (unsigned int i = threadIdleIntervals(interval); i && ret < 0; i--) {
	Thread::idle();
	ret = ::waitpid(pid,0,WNOHANG);
    }
    return ret;
}

void YBTSDriver::stopPeer()
{
    if (!m_peerPid)
	return;
    int w = ::waitpid(m_peerPid,0,WNOHANG);
    if (w > 0) {
	Debug(this,DebugInfo,"Peer pid %d already terminated",m_peerPid);
	m_peerPid = 0;
	return;
    }
    if (w == 0) {
	Debug(this,DebugNote,"Peer pid %d has not exited - we'll kill it",m_peerPid);
	::kill(m_peerPid,SIGTERM);
	w = waitPid(m_peerPid,100);
    }
    if (w == 0) {
	Debug(this,DebugWarn,"Peer pid %d has still not exited yet?",m_peerPid);
	::kill(m_peerPid,SIGKILL);
	w = waitPid(m_peerPid,60);
	if (w <= 0)
	    Alarm(this,"system",DebugWarn,"Failed to kill peer pid %d, leaving zombie",m_peerPid);
    }
    else if (w < 0 && errno != ECHILD)
	Debug(this,DebugMild,"Failed waitpid on peer pid %d: %d %s",
	    m_peerPid,errno,::strerror(errno));
    else
	Debug(this,DebugInfo,"Peer pid %d terminated",m_peerPid);
    m_peerPid = 0;
}

static inline void setHalfByteDigits(uint8_t*& buf, uint8_t value)
{
    if (value < 100) {
	if (value < 10)
	    *buf = (value << 4);
	else
	    *buf = ((value % 10) << 4) | (value / 10);
    }
    buf++;
}

bool YBTSDriver::handleMsgExecute(Message& msg, const String& dest)
{
    if ((m_state != RadioUp) || !m_mm) {
	Debug(this,DebugWarn,"MT SMS: Radio is not up!");
	msg.setParam(s_error,"interworking");
	return false;
    }
    RefPointer<YBTSUE> ue;
    if (!m_mm->findUESafe(ue,dest)) {
	// We may consider paging UEs that are not in the TMSI table
	msg.setParam(s_error,"offline");
	return false;
    }
    if (!m_signalling) {
	msg.setParam(s_error,"interworking");
	return false;
    }
    NamedString* rpdu = msg.getParam(YSTRING("rpdu"));
    if (!rpdu)
	rpdu = msg.getParam(YSTRING("xsip_body"));
    NamedString tmpRpdu(rpdu ? rpdu->name().c_str() : "rpdu");
    while (TelEngine::null(rpdu)) {
	const String& type = msg[YSTRING("operation")];
	if (type && type != YSTRING("deliver")) {
	    Debug(this,DebugNote,"MT SMS: unknown operation '%s'",type.c_str());
	    break;
	}
	String enc = msg[YSTRING("text.encoding")];
	if (enc && enc.toLower() != YSTRING("gsm7bit")) {
	    Debug(this,DebugNote,"MT SMS: unknown encoding '%s'",enc.c_str());
	    break;
	}
	const String& text = msg[YSTRING("text")];
	if (!text) {
	    Debug(this,DebugNote,"MT SMS: empty text");
	    break;
	}
	DataBlock rp;
	uint8_t n = 0;
	// ETSI TS 124.011 Section 7.3 and 8.2:
	// 1 byte (3 bits): message type
	// 1 byte message reference
	uint8_t hdr[2] = {RPDataFromNetwork,0};
	rp.assign(hdr,2);
	// RP-Originator Address
	const String& caller = msg[YSTRING("caller")];
	uint8_t* c = new uint8_t[2 + (caller.length() + 1) / 2];
	unsigned int l = encodeBCDNumber(c + 1,caller.length(),caller,
	    msg.getValue(YSTRING("callernumplan")),msg.getValue(YSTRING("callernumtype")));
	// ETSI TS 124.011 Section 8.2.5.1: max len of IE is 11
	if (l > 11) {
	    delete[] c;
	    Debug(this,DebugNote,"MT SMS: invalid caller '%s' length",caller.c_str());
	    break;
	}
	// Empty caller: put something there, section 8.2.5.1 says IE len is at least 2
	if (l == 1) {
	    c[2] = 0xf0;
	    l = 2;
	}
	c[0] = l;
	rp.append(c,l + 1);
	delete[] c;
	// RP-Destination Address
	rp.append(&n,1);
	// RP-User Data: IE length + TPDU
	// TPDU: SMS DELIVER, ETSI TS 123.040 Section 9.2.2.1
	DataBlock tpdu;
	// TP-Originating-Address
	const String& smsCaller = msg[YSTRING("sms.caller")];
	uint8_t* orig = new uint8_t[2 + (smsCaller.length() + 1) / 2];
	unsigned int origLen = encodeBCDNumber(orig + 1,smsCaller.length(),smsCaller,
	    msg.getValue(YSTRING("sms.caller.plan")),
	    msg.getValue(YSTRING("sms.caller.nature")));
	// TS 123.040 Section 9.1.2.5: max len is 12
	if (origLen + 1 > 12) {
	    delete[] orig;
	    Debug(this,DebugNote,"MT SMS: invalid SMS caller '%s' length",
		smsCaller.c_str());
	    break;
	}
	uint8_t nDigits = (origLen - 1) * 2;
	if ((orig[origLen] & 0xf0) == 0xf0)
	    nDigits--;
	orig[0] = nDigits;
	tpdu.append(orig,origLen + 1);
	delete[] orig;
	// TP-Protocol-Identifier + TP-Coding-Scheme + TP-Service-Centre-Time-Stamp
	uint8_t extra[9] = {0,0,0,0,0,0,0,0,0};
	// TP-Service-Centre-Time-Stamp
	unsigned int smscTs = msg.getIntValue(YSTRING("smsc.timestamp"),Time::secNow(),0);
	extra[8] = (uint8_t)msg.getIntValue(YSTRING("smsc.tz"));
	int year = 0;
	unsigned int month = 0;
	unsigned int day = 0;
	unsigned int hour = 0;
	unsigned int minute = 0;
	unsigned int sec = 0;
	if (Time::toDateTime(smscTs,year,month,day,hour,minute,sec) && year >= 0) {
	    uint8_t* ts = &extra[2];
	    setHalfByteDigits(ts,year % 100);
	    setHalfByteDigits(ts,month);
	    setHalfByteDigits(ts,day);
	    setHalfByteDigits(ts,hour);
	    setHalfByteDigits(ts,minute);
	    setHalfByteDigits(ts,sec);
	}
	tpdu.append(extra,9);
	// TP-User-Data-Length + TP-User-Data
	DataBlock sms;
	GSML3Codec::encodeGSM7Bit(text,sms);
	if (!sms.length()) {
	    Debug(this,DebugNote,"MT SMS: text leads to empty SMS content");
	    break;
	}
	uint8_t smsLen = sms.length() * 8 / 7;
	tpdu.append(&smsLen,1);
	tpdu += sms;
	// TS 124.011: RP-User-Data IE max len in RP-DATA is 233
	l = tpdu.length() + 1;
	if (l > 232) {
	    Debug(this,DebugNote,"MT SMS: RP-User-Data too long %u (max 233)",l);
	    break;
	}
	uint8_t rpUserHdr[2] = {l,0x04};
	rp.append(rpUserHdr,2);
	rp += tpdu;
	tmpRpdu.hexify(rp.data(),rp.length());
	rpdu = &tmpRpdu;
	break;
    }
    if (TelEngine::null(rpdu)) {
	Debug(this,DebugNote,"MT SMS to IMSI=%s TMSI=%s: no RPDU",
	    ue->imsi().c_str(),ue->tmsi().c_str());
	msg.setParam(s_error,"failure");
	return false;
    }
    YBTSMtSms* sms = 0;
    RefPointer<YBTSMtSmsList> list;
    if (findMtSmsList(list,ue,true)) {
	String s = msg[YSTRING("id")];
	if (!s)
	    setSmsId(s);
	sms = new YBTSMtSms(s,*rpdu);
	if (sms->ref()) {
	    Lock lck(list);
	    list->m_sms.append(sms);
	}
	else {
	    TelEngine::destruct(sms);
	    removeMtSms(list);
	    list = 0;
	}
    }
    if (!list) {
    	Debug(this,DebugMild,"MT SMS to IMSI=%s TMSI=%s: ref() failed",
	    ue->imsi().c_str(),ue->tmsi().c_str());
	msg.setParam(s_error,"failure");
	return false;
    }
    Debug(this,DebugInfo,"MT SMS '%s' to IMSI=%s",
	sms->id().c_str(),ue->imsi().c_str());
    checkMtSms(*list);
    unsigned int intervals = msg.getIntValue(YSTRING("timeout"),s_mtSmsTimeout,
	YBTS_MT_SMS_TIMEOUT_MIN,YBTS_MT_SMS_TIMEOUT_MAX);
    intervals = threadIdleIntervals(intervals);
    while (sms->active()) {
#define YBTS_SMSOUT_DONE(reason) { sms->terminate(false,reason); break; }
	Thread::idle();
	if (Engine::exiting())
	    YBTS_SMSOUT_DONE("exiting");
	if (Thread::check(false))
	    YBTS_SMSOUT_DONE("cancelled");
	if (intervals) {
	    intervals--;
	    if (!intervals)
		YBTS_SMSOUT_DONE("timeout");
	}
	if (list->ue()->removed() || list->ue()->imsiDetached())
	    YBTS_SMSOUT_DONE("offline");
	// Stop waiting if YBTS was stopped without restart
	if (m_state == Idle && m_error)
	    YBTS_SMSOUT_DONE("failure");
	if (list->m_check) {
	    list->lock();
	    bool check = list->m_check;
	    list->m_check = false;
	    list->unlock();
	    if (check)
		checkMtSms(*list);
	}
#undef YBTS_SMSOUT_DONE
    }
    bool ok = sms->ok();
    msg.setParam(rpdu->name(),sms->response());
    if (!ok)
	msg.setParam(s_error,sms->reason().safe("failure"));
    if (ok)
	Debug(this,DebugInfo,"MT SMS '%s' to IMSI=%s finished",
	    sms->id().c_str(),ue->imsi().c_str());
    else
	Debug(this,DebugInfo,"MT SMS '%s' to IMSI=%s failed reason='%s' RPDU=%s",
	    sms->id().c_str(),ue->imsi().c_str(),
	    sms->reason().safe(),sms->response().safe());
    removeMtSms(list,sms);
    TelEngine::destruct(sms);
    list = 0;
    return ok;
}

bool YBTSDriver::handleEngineStop(Message& msg)
{
    m_engineStop++;
    dropAll(msg);
    dropAllSS();
    if (m_signalling)
	m_signalling->dropAllSS();
    bool haveThreads = false;
    if (m_engineStop == 1) {
	haveThreads = !YBTSGlobalThread::cancelAll();
	// TODO: unregister
    }
    else {
	m_media->cleanup(false);
	haveThreads = !YBTSGlobalThread::cancelAll(false,60);
    }
    bool canStop = !(haveThreads || m_haveCalls);
    if (canStop) {
	Lock lck(this);
	canStop = (channels().skipNull() == 0);
    }
    if (canStop)
	stop();
    return !canStop;
}

YBTSChan* YBTSDriver::findChanConnId(uint16_t connId)
{
    for (ObjList* o = channels().skipNull(); o ; o = o->skipNext()) {
	YBTSChan* ch = static_cast<YBTSChan*>(o->get());
	if (ch->connId() == connId)
	    return ch;
    }
    return 0;
}

YBTSChan* YBTSDriver::findChanUE(const YBTSUE* ue)
{
    for (ObjList* o = channels().skipNull(); o ; o = o->skipNext()) {
	YBTSChan* ch = static_cast<YBTSChan*>(o->get());
	if (ch->ue() == ue)
	    return ch;
    }
    return 0;
}

void YBTSDriver::changeState(int newStat)
{
    if (m_state == newStat)
	return;
    String extra;
    if (newStat == Starting)
	extra << " restart counter " << m_restartIndex << "/" << s_restartMax;
    Debug(this,DebugNote,"State changed %s -> %s%s",
	stateName(),lookup(newStat,s_stateName),extra.safe());
    m_state = newStat;
    // Update globals
    Lock lck(s_globalMutex);
    if (m_state != Idle) {
	s_startTime = Time::now();
	s_idleTime = 0;
    }
    else {
	s_startTime = 0;
	s_idleTime = Time::now();
    }
}

void YBTSDriver::setRestart(int resFlag, bool on, unsigned int intervalMs)
{
    if (resFlag >= 0)
	m_restart = (resFlag > 0);
    if (on) {
	if (!intervalMs)
	    intervalMs = s_restartMs;
	m_restartTime = Time::now() + (uint64_t)intervalMs  * 1000;
	Debug(this,DebugAll,"Restart scheduled in %ums [%p]",intervalMs,this);
    }
    else if (m_restartTime) {
	m_restartTime = 0;
	Debug(this,DebugAll,"Restart timer reset [%p]",this);
    }
}

// Check stop conditions
void YBTSDriver::checkStop(const Time& time)
{
    Lock lck(m_stateMutex);
    if (m_stop && m_stopTime) {
	if (m_stopTime <= time) {
	    lck.drop();
	    stop();
	}
    }
    else
	m_stop = false;
}

// Check restart conditions
void YBTSDriver::checkRestart(const Time& time)
{
    Lock lck(m_stateMutex);
    if (m_restart && m_restartTime) {
	if (m_restartTime <= time) {
	    lck.drop();
	    start();
	}
    }
    else
	m_restart = false;
}

void YBTSDriver::ybtsStatus(String& line, String& retVal)
{
    Module::statusModule(retVal);
    Lock lck(m_stateMutex);
    retVal.append("state=",";");
    retVal << stateName();
    uint64_t val = 0;
    if (state() != Idle)
	getGlobalUInt64(val,s_startTime);
    else {
	if (m_stopped)
	    retVal << " (stopped)";
	getGlobalUInt64(val,s_idleTime);
    }
    lck.drop();
    retVal << ",state_time=" << (val ? ((Time::now() - val) / 1000000)  : 0);
    retVal << "\r\n";
}

void YBTSDriver::btsStatus(Message& msg)
{
    msg.retValue() << "name=" << BTS_CMD << ",type=misc;state=" << stateName() << "\r\n";
}

void YBTSDriver::initialize()
{
    static bool s_first = true;
    Output("Initializing module YBTS");
    Configuration cfg(Engine::configFile("ybts"));
    const NamedList& gsm = safeSect(cfg,YSTRING("gsm"));
    YBTSLAI lai;
    String mcc = gsm.getValue(YSTRING("Identity.MCC"),YBTS_MCC_DEFAULT);
    String mnc = gsm.getValue(YSTRING("Identity.MNC"),YBTS_MNC_DEFAULT);
    String lac = gsm.getValue(YSTRING("Identity.LAC"),YBTS_LAC_DEFAULT);
    if (mcc.length() == 3 && isDigits09(mcc) &&
	((mnc.length() == 3 || mnc.length() == 2) && isDigits09(mnc))) {
	int tmp = lac.toInteger(-1);
	if (tmp >= 0 && tmp <= 0xff00) {
	    uint8_t b[2];
	    b[0] = tmp >> 8;
	    b[1] = tmp;
	    lac.hexify(b,2);
	    lai.set(mcc,mnc,lac);
	}
    }
    const NamedList& ybts = safeSect(cfg,YSTRING("ybts"));
    s_restartMax = ybts.getIntValue("max_restart",
	YBTS_RESTART_COUNT_DEF,YBTS_RESTART_COUNT_MIN);
    s_restartMs = ybts.getIntValue("restart_interval",
	YBTS_RESTART_DEF,YBTS_RESTART_MIN,YBTS_RESTART_MAX);
    s_t305 = ybts.getIntValue("t305",30000,20000,60000);
    s_t308 = ybts.getIntValue("t308",5000,4000,20000);
    s_t313 = ybts.getIntValue("t313",5000,4000,20000);
    s_tmsiExpire = ybts.getIntValue("tmsi_expire",864000,7200,2592000);
    s_tmsiSave = ybts.getBoolValue("tmsi_save");
    s_mtSmsTimeout = ybts.getIntValue("sms.timeout",
	YBTS_MT_SMS_TIMEOUT_DEF,YBTS_MT_SMS_TIMEOUT_MIN,YBTS_MT_SMS_TIMEOUT_MAX);
    s_ussdTimeout = ybts.getIntValue("ussd.session_timeout",
	YBTS_USSD_TIMEOUT_DEF,YBTS_USSD_TIMEOUT_MIN);
    const String& expXml = ybts[YSTRING("export_xml_as")];
    if (expXml == YSTRING("string"))
	m_exportXml = -1;
    else if (expXml == YSTRING("both"))
	m_exportXml = 0;
    else
	m_exportXml = 1;
    s_globalMutex.lock();
    if (lai.lai()) {
	if (lai != s_lai || !s_lai.lai()) {
	    Debug(this,DebugInfo,"LAI changed %s -> %s",
		s_lai.lai().c_str(),lai.lai().c_str());
	    s_lai = lai;
	}
    }
    else {
	Debug(this,DebugConf,"Invalid LAI MNC='%s' MCC='%s' LAC='%s'",
	    mnc.c_str(),mcc.c_str(),lac.c_str());
	s_lai.reset();
    }
    s_askIMEI = ybts.getBoolValue("imei_request",true);
    s_ueFile = ybts.getValue("datafile",Engine::configFile("ybtsdata"));
    Engine::runParams().replaceParams(s_ueFile);
    s_peerCmd = ybts.getValue("peer_cmd","${modulepath}/" BTS_DIR "/" BTS_CMD);
    s_peerArg = ybts.getValue("peer_arg");
    s_peerDir = ybts.getValue("peer_dir","${modulepath}/" BTS_DIR);
    Engine::runParams().replaceParams(s_peerCmd);
    Engine::runParams().replaceParams(s_peerArg);
    Engine::runParams().replaceParams(s_peerDir);
#ifdef DEBUG
    String s;
    s << "\r\nLAI=" << s_lai.lai();
    s << "\r\nimei_request=" << String::boolText(s_askIMEI);
    s << "\r\ntmsi_expire=" << s_tmsiExpire << "s";
    s << "\r\ndatafile=" << s_ueFile;
    s << "\r\nmax_restart=" << s_restartMax;
    s << "\r\nt305=" << s_t305;
    s << "\r\nt308=" << s_t308;
    s << "\r\nt313=" << s_t313;
    s << "\r\nsms.timeout=" << s_mtSmsTimeout;
    s << "\r\nussd.session_timeout=" << s_ussdTimeout;
    s << "\r\npeer_cmd=" << s_peerCmd;
    s << "\r\npeer_arg=" << s_peerArg;
    s << "\r\npeer_dir=" << s_peerDir;
    s << "\r\nexport_xml_as=" << (m_exportXml > 0 ? "object" : (m_exportXml ? "string" : "both"));
    Debug(this,DebugAll,"Initialized\r\n-----%s\r\n-----",s.c_str());
#endif
    s_globalMutex.unlock();
    if (s_first) {
	s_first = false;
	setup();
	installRelay(MsgExecute);
	installRelay(Halt);
	installRelay(Stop,"engine.stop");
	installRelay(Start,"engine.start");
	YBTSMsgHandler::install();
	s_configFile = cfg;
        m_logTrans = new YBTSLog("transceiver");
        m_logBts = new YBTSLog(BTS_CMD);
        m_command = new YBTSCommand;
	m_media = new YBTSMedia;
	m_signalling = new YBTSSignalling;
	m_mm = new YBTSMM(ybts.getIntValue("ue_hash_size",31,5));
	m_mm->loadUElist();
    }
    m_signalling->init(cfg);
    startIdle();
}

bool YBTSDriver::msgExecute(Message& msg, String& dest)
{
    if (!msg.userData()) {
	Debug(this,DebugWarn,"GSM call found but no data channel!");
	return false;
    }
    if ((m_state != RadioUp) || !m_mm) {
	Debug(this,DebugWarn,"GSM call: Radio is not up!");
	msg.setParam(s_error,"interworking");
	return false;
    }
    RefPointer<YBTSUE> ue;
    if (!m_mm->findUESafe(ue,dest)) {
	// We may consider paging UEs that are not in the TMSI table
	msg.setParam(s_error,"offline");
	return false;
    }
    Debug(this,DebugCall,"MT call to IMSI=%s TMSI=%s",ue->imsi().c_str(),ue->tmsi().c_str());
    RefPointer<YBTSChan> chan;
    if (findChan(ue,chan)) {
	if (!chan->canAcceptMT()) {
	    msg.setParam(s_error,"busy");
	    return false;
	}
	// TODO
	Debug(this,DebugStub,"CW call not implemented");
	return false;
    }
    if (!m_signalling)
	return false;
    RefPointer<YBTSConn> conn;
    if (m_signalling->findConn(conn,ue)) {
	// TODO
	Debug(this,DebugStub,"Call on existing connection not implemented");
	return false;
    }
    chan = new YBTSChan(ue);
    chan->deref();
    if (chan->initOutgoing(msg)) {
	CallEndpoint* ch = YOBJECT(CallEndpoint,msg.userData());
	if (ch && chan->connect(ch,msg.getValue(YSTRING("reason")))) {
	    chan->callConnect(msg);
	    msg.setParam("peerid",chan->id());
	    msg.setParam("targetid",chan->id());
	    return true;
	}
    }
    return false;
}

bool YBTSDriver::received(Message& msg, int id)
{
    switch (id) {
	case MsgExecute:
	    {
		const String& dest = msg[YSTRING("callto")];
		return dest.startsWith(prefix()) &&
		    handleMsgExecute(msg,dest.substr(prefix().length()));
	    }
	case Start:
	    if (!m_engineStart) {
		m_engineStart = true;
		startIdle();
	    }
	    return false;
	case Stop:
	    return handleEngineStop(msg);
	case Halt:
	    dropAll(msg);
	    stop();
	    YBTSGlobalThread::cancelAll(true);
	    YBTSMsgHandler::uninstall();
	    break;
	case Timer:
	    // Handle stop/restart
	    // Don't handle both in the same tick: wait some time between stop and restart
	    if (m_stop)
		checkStop(msg.msgTime());
	    else if (m_restart)
		checkRestart(msg.msgTime());
	    if (m_signalling->shouldCheckTimers()) {
		int res = m_signalling->checkTimers(msg.msgTime());
		if (res) {
		    if (res == YBTSSignalling::FatalError)
			stopNoRestart();
		    else
			restart();
		}
	    }
	    if (m_haveCalls)
		checkTerminatedCalls(msg.msgTime());
	    if (m_mm)
		m_mm->checkTimers(msg.msgTime());
	    if (m_mtSsNotEmpty)
		checkMtSsTout(msg.msgTime());
	    if (isPeerCheckState()) {
		pid_t pid = 0;
		Lock lck(m_stateMutex);
		if (isPeerCheckState()) {
		    if (m_peerAlive || !m_peerCheckTime)
			m_peerCheckTime = msg.msgTime() +
			    (uint64_t)m_peerCheckIntervalMs * 1000;
		    else if (m_peerCheckTime <= msg.msgTime())
			pid = m_peerPid;
		    m_peerAlive = false;
		}
		lck.drop();
		int res = pid ? ::waitpid(pid,0,WNOHANG) : 0;
		if (res > 0 || (res < 0 && errno == ECHILD)) {
		    lck.acquire(m_stateMutex);
		    if (pid == m_peerPid) {
			Debug(this,DebugNote,"Peer pid %d vanished",m_peerPid);
			m_restartTime = 0;
			lck.drop();
			restart();
		    }
		}
	    }
	    break;
	case Status:
	    {
		String dest = msg.getValue(YSTRING("module"));
		if (dest.null() || dest == YSTRING(BTS_CMD)) {
		    btsStatus(msg);
		    if (dest)
			return true;
		}
	    }
	    break;
    }
    return Driver::received(msg,id);
}

bool YBTSDriver::commandExecute(String& retVal, const String& line)
{
    String tmp = line;
    if (tmp.startSkip(name())) {
	if (tmp.startSkip(s_statusCmd))
	    ybtsStatus(tmp,retVal);
	else if (tmp.startSkip(s_startCmd)) {
	    cmdStartStop(true);
	    startIdle();
	}
	else if (tmp.startSkip(s_stopCmd)) {
	    cmdStartStop(false);
	    stopNoRestart();
	}
	else if (tmp.startSkip(s_restartCmd)) {
	    cmdStartStop(true);
	    restart(tmp.toInteger(1,0,0));
	}
	else
	    return Driver::commandExecute(retVal,line);
	return true;
    }
    if (m_command && tmp.startSkip(BTS_CMD)) {
	if (tmp.trimSpaces().null())
	    return false;
	if (!m_command->send(tmp))
	    return false;
	if (!m_command->recv(tmp))
	    return false;
	// LF -> CR LF
	for (int i = 0; (i = tmp.find('\n',i + 1)) >= 0; ) {
	    if (tmp.at(i - 1) != '\r') {
		tmp = tmp.substr(0,i) + "\r" + tmp.substr(i);
		i++;
	    }
	}
	if (!tmp.endsWith("\n"))
	    tmp << "\r\n";
	retVal = tmp;
	return true;
    }
    return Driver::commandExecute(retVal,line);
}

bool YBTSDriver::commandComplete(Message& msg, const String& partLine, const String& partWord)
{
    if (partLine.null()) {
	itemComplete(msg.retValue(),YSTRING(BTS_CMD),partWord);
	itemComplete(msg.retValue(),name(),partWord);
    }
    else if (partLine == name()) {
	itemComplete(msg.retValue(),s_statusCmd,partWord);
	itemComplete(msg.retValue(),s_startCmd,partWord);
	itemComplete(msg.retValue(),s_stopCmd,partWord);
	itemComplete(msg.retValue(),s_restartCmd,partWord);
    }
    if (partLine == YSTRING("debug"))
	itemComplete(msg.retValue(),YSTRING("transceiver"),partWord);
    if ((partLine == YSTRING("debug")) || (partLine == YSTRING("status")))
	itemComplete(msg.retValue(),YSTRING(BTS_CMD),partWord);
    if (m_command && ((partLine == YSTRING(BTS_CMD)) || (partLine == YSTRING(BTS_CMD " help")))) {
	if (!m_helpCache && (m_state >= Running) && lock(100000)) {
	    String tmp;
	    if (m_command->send("help") && m_command->recv(tmp) && tmp.trimBlanks()) {
		ObjList* help = new ObjList;
		ObjList* app = help;
		ObjList* lines = tmp.split('\n');
		for (ObjList* l = lines; l; l = l->skipNext()) {
		    ObjList* words = l->get()->toString().split('\t',false);
		    if (words->get() && !words->get()->toString().startsWith("Type")) {
			while (GenObject* o = words->remove(false))
			    app = app->append(o);
		    }
		    TelEngine::destruct(words);
		}
		TelEngine::destruct(lines);
		m_helpCache = help;
	    }
	    unlock();
	}
	if (m_helpCache) {
	    for (ObjList* l = m_helpCache->skipNull(); l; l = l->skipNext())
		itemComplete(msg.retValue(),l->get()->toString(),partWord);
	}
	return true;
    }
    return Driver::commandComplete(msg,partLine,partWord);
}

bool YBTSDriver::setDebug(Message& msg, const String& target)
{
    if (target == YSTRING("transceiver"))
	return m_logTrans && m_logTrans->setDebug(msg,target);
    if (target == YSTRING(BTS_CMD))
	return m_logBts && m_logBts->setDebug(msg,target);
    return Driver::setDebug(msg,target);
}


//
// YBTSMsgHandler
//
YBTSMsgHandler::YBTSMsgHandler(int type, const char* name, int prio)
    : MessageHandler(name,prio,__plugin.name()),
    m_type(type)
{
}

bool YBTSMsgHandler::received(Message& msg)
{
    switch (m_type) {
	case UssdUpdate:
	case UssdFinalize:
	    return !__plugin.sameModule(msg) &&
		__plugin.handleUssd(msg,m_type == UssdUpdate);
	case UssdExecute:
	    if (!__plugin.sameModule(msg)) {
		String dest = msg[YSTRING("callto")];
		if (dest.startSkip(__plugin.prefix(),false))
		    return __plugin.handleUssdExecute(msg,dest);
	    }
	    return false;
    }
    Debug(&__plugin,DebugStub,"YBTSMsgHandler::received() not implemented for %d",m_type);
    return false;
}

void YBTSMsgHandler::install()
{
    if (s_handlers.skipNull())
	return;
    for (const TokenDict* d = s_name; d->token; d++) {
	int prio = d->value < 0 ? 100 : d->value;
	YBTSMsgHandler* h = new YBTSMsgHandler(d->value,d->token,prio);
	if (Engine::install(h))
	    s_handlers.append(h)->setDelete(false);
	else {
	    TelEngine::destruct(h);
	    Alarm(&__plugin,"system",DebugWarn,"Failed to install handler for '%s'",d->token);
	}
    }
}

void YBTSMsgHandler::uninstall()
{
    for (ObjList* o = s_handlers.skipNull(); o; o = o->skipNull()) {
	YBTSMsgHandler* h = static_cast<YBTSMsgHandler*>(o->remove(false));
	if (Engine::uninstall(h))
	    TelEngine::destruct(h);
    }
}

}; // anonymous namespace

/* vi: set ts=8 sw=4 sts=4 noet: */
