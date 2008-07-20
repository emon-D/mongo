// protoimpl.h

/**
*    Copyright (C) 2008 10gen Inc.
*  
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*  
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*  
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

/* packet dumping level of detail. */
const bool dumpPackets = false; // this must be true to get anything at all
const bool dumpIP = false; // output the ip address
const bool dumpBytesDetailed = false; // more data output

#include "message.h"

extern boost::mutex coutmutex;

const int FragMax = 1480;
const int FragHeader = 10;
const int MSS = FragMax - FragHeader;

#pragma pack(push)
#pragma pack(1)

struct Fragment {
	enum { MinFragmentLen = FragHeader + 1 };
	MSGID msgId;
	short channel;
	short fragmentLen;
	short fragmentNo;
	char data[16];
	int fragmentDataLen() { return fragmentLen - FragHeader; }
	char* fragmentData() { return data; }

	bool ok(int nRead) { 
		if( nRead < MinFragmentLen || fragmentLen > nRead || fragmentLen < MinFragmentLen ) {
			ptrace( cout << ".recv: fragment bad. fragmentLen:" << fragmentLen << " nRead:" << nRead << endl; )
			return false;
		}
		if( fragmentNo == 0 && fragmentLen < MinFragmentLen + MsgDataHeaderSize ) { 
			ptrace( cout << ".recv: bad first fragment. fragmentLen:" << fragmentLen << endl; )
			return false;
		}
		return true;
	} 

	MsgData* startOfMsgData() { assert(fragmentNo == 0); return (MsgData *) data; }
};
#pragma pack(pop)

inline void DUMP(Fragment& f, SockAddr& t, const char *tabs) { 
	if( !dumpPackets )
		return;
	cout << tabs << curTimeMillis() % 10000 << ' ';
	short s = f.fragmentNo;
	if( s == -32768 ) 
		cout << "ACK M:" << f.msgId % 1000;
	else if( s == -32767 )
		cout << "MISSING";
	else if( s == -32766 )
		cout << "RESET ch:" << f.channel;
	else if( s < 0 )
		cout << "REQUESTACK";
	else
		cout << '#' << s << ' ' << f.fragmentLen << " M:" << f.msgId % 1000;
	cout << ' ';
	if( dumpIP )
		cout << t.toString();
}

inline void DUMPDATA(Fragment& f, const char *tabs) { 
	if( !dumpPackets )
		return;
	if( f.fragmentNo >= 0 ) { 
		cout << '\n' << tabs;
		int x = f.fragmentDataLen();
		if( dumpBytesDetailed ) { 
			char *p = (char *) &f;
			cout << hex << *((unsigned*)p) << ' '; p+=4;
			cout << *((short*)p) << ' '; p+=2;
			cout << *((short*)p) << ' '; p+=2;
			cout << *((short*)p) << '|'; p+=2;
			if( x < 16 ) cout << "???";
			else {
				for( int i = 0; i < 4; i++ ) { // MSGDATA
					cout << *((unsigned*)p);
					cout << (i < 3 ? ' ' : '|');
					p += 4;
				}
				cout << '\n' << tabs;
				x -= 16;
				if( x > 32 ) x = 32;
				while( x-- > 0 ) { cout << (unsigned) (unsigned char) *p++ << ' '; } 
			}
		}
		else { 
			char *p = f.data;
			if( f.fragmentNo == 0 ) { 
				p += 16; x -= 16;
			}
			if( x > 28 ) x = 28;
			for( int i = 0; i < x; i++ ) {
				if( *p == 0 ) cout << (char) 0xb0;
				else cout << (*p >= 32 ? *p : '.');
				p++;
			}
		}
	}
	cout << dec << endl;
}

inline void SEND(UDPConnection& c, Fragment &f, SockAddr& to, const char *extra="") { 
	lock lk(coutmutex);
	DUMP(f, to, "\t\t\t\t\t>");
	c.sendto((char *) &f, f.fragmentLen, to);
	if( dumpPackets )
		cout << extra;
	DUMPDATA(f, "\t\t\t\t\t      ");
}

// sender ->
inline void __sendFrag(ProtocolConnection *pc, EndPoint& to, F *f, bool retran) {
	assert( f->internals->channel == to.channel );
    ptrace( cout << ".sendfrag " << f->__num() << ' ' << retran << endl; )
	SEND(pc->udpConnection, *f->internals, to.sa, retran ? " retran" : "");
}

inline void __sendREQUESTACK(ProtocolConnection *pc, EndPoint& to, 
							 MSGID msgid, int fragNo) { 
	Fragment f;
	f.msgId = msgid;
	f.channel = to.channel; assert( f.channel >= 0 );
	f.fragmentNo = ((short) -fragNo) -1;
	f.fragmentLen = FragHeader;
	ptrace( cout << ".requesting ack, fragno=" << f.fragmentNo << " msg:" << f.msgId << ' ' << to.toString() << endl; )
	SEND(pc->udpConnection, f, to.sa);
}

// receiver -> 
inline void __sendACK(ProtocolConnection *pc, MSGID msgid) {
	ptrace( cout << "...__sendACK() to:" << pc->farEnd.toString() << " msg:" << msgid << endl; )
	Fragment f;
	f.msgId = msgid;
	f.channel = pc->farEnd.channel; assert( f.channel >= 0 );
	f.fragmentNo = -32768;
	f.fragmentLen = FragHeader;
	SEND(pc->udpConnection, f, pc->farEnd.sa);
}

/* this is to clear old state for the channel in terms of what msgids are 
   already sent. 
*/
inline void __sendRESET(ProtocolConnection *pc) {
	Fragment f;
	f.msgId = -1;
	f.channel = pc->farEnd.channel; assert( f.channel >= 0 );
	f.fragmentNo = -32766;
	f.fragmentLen = FragHeader;
	ptrace( cout << "...__sendRESET() to:" << pc->farEnd.toString() << endl; )
	SEND(pc->udpConnection, f, pc->farEnd.sa);
}

inline void __sendMISSING(ProtocolConnection *pc, EndPoint& to, 
						  MSGID msgid, vector<short>& ids) {
	int n = ids.size(); 
	ptrace( cout << "..sendMISSING n:" << n << " firstmissing:" << ids[0] << " last:" << ids[ids.size()-1] << to.toString() << endl; )
	if( n > 256 ) {
		ptrace( cout << "\t..sendMISSING limiting to 256 ids" << endl; )
		n = 256;
	}
	Fragment *f = (Fragment*) malloc(FragHeader + n*2);
	f->msgId = msgid;
	f->channel = to.channel; assert( f->channel >= 0 );
	f->fragmentNo = -32767;
	f->fragmentLen = FragHeader + n*2;
	short *s = (short *) f->data;
	for( int i = 0; i < n; i++ )
		*s++ = ids[i];
//	ptrace( cout << "...sendMISSING fraglen:" << f->fragmentLen << endl; )
	SEND(pc->udpConnection, *f, to.sa);
	free(f);
}

// -> receiver
inline F* __recv(UDPConnection& c, SockAddr& from) {
	Fragment *f = (Fragment *) malloc(MaxMTU);
	int n;
	while( 1 ) {
//		n = c.recvfrom((char*) f, c.mtu(), from);
		n = c.recvfrom((char*) f, MaxMTU, from);
//		cout << "recvfrom returned " << n << endl;
		if( n >= 0 )
			break;
		if( !goingAway ) {
			cout << ".recvfrom returned error " << getLastError() << " socket:" << c.sock << endl;
			cout << "sleeping 2 seconds " << endl;
			sleepsecs(2);
		}
	}
	assert( f->fragmentLen == n );
	if( f->fragmentNo > 0 ) {
		// don't waste tons of space if the maxmtu is 16k but we get 1480
		unsigned newsz = (f->fragmentLen + 255) & 0xffffff00;
		if( newsz < MaxMTU )
			f = (Fragment *) realloc(f, newsz);
	}
	{
		lock lk(coutmutex);
		DUMP(*f, from, "\t\t\t\t\t\t\t\t\t\t<");
		DUMPDATA(*f,   "\t\t\t\t\t\t\t\t\t\t      ");
	}
	return new F(f);
}

inline F::F(Fragment *f) : internals(f), op(NORMAL) { 
	if( internals->fragmentNo < 0 ) { 
		if( internals->fragmentNo == -32768 ) {
			op = ACK;
			ptrace( cout << ".got ACK msg:" << internals->msgId << endl; )
		} else if( internals->fragmentNo == -32767 ) {
			op = MISSING;
			ptrace( cout << ".got MISSING" << endl; )
		} else if( internals->fragmentNo == -32766 ) {
			op = RESET;
		} else {
			op = REQUESTACK;
			internals->fragmentNo = -(internals->fragmentNo+1);
			ptrace( cout << ".got REQUESTACK frag:" << internals->fragmentNo << " msg:" << internals->msgId << endl; )
		}
	}
}
inline F::~F() { free(internals); internals=0; }
inline int F::__num() { return internals->fragmentNo; }
inline int F::__len() { return internals->fragmentLen; }
inline MSGID F::__msgid() { return internals->msgId; }
inline int F::__channel() { return internals->channel; }
inline bool F::__isREQUESTACK() { return op == REQUESTACK; }
inline bool F::__isACK() { return op == ACK; }
inline bool F::__isMISSING() { return op == MISSING; }
inline short* F::__getMissing(int& n) { 
	n = internals->fragmentDataLen() / 2;
	return (short *) internals->fragmentData();
}
inline int F::__firstFragMsgLen() { 
	return internals->startOfMsgData()->len;
}
