#include "config.h"
#include "wforce.hh"
#include <thread>
#include "dolog.hh"
#include "sodcrypto.hh"
#include "base64.hh"
#include "twmap.hh"
#include <fstream>

#ifdef HAVE_GEOIP
#include "wforce-geoip.hh"
#endif
#ifdef HAVE_GETDNS
#include "dns_lookup.hh"
#endif

using std::thread;

static vector<std::function<void(void)>>* g_launchWork;

vector<std::function<void(void)>> setupLua(bool client, const std::string& config)
{
  g_launchWork= new vector<std::function<void(void)>>();
  g_lua.writeFunction("addACL", [](const std::string& domain) {
      g_ACL.modify([domain](NetmaskGroup& nmg) { nmg.addMask(domain); });
    });

  g_lua.writeFunction("addSibling", [](const std::string& address) {
      ComboAddress ca(address, 4001);
      g_siblings.modify([ca](vector<shared_ptr<Sibling>>& v) { v.push_back(std::make_shared<Sibling>(ca)); });
    });

  g_lua.writeFunction("setSiblings", [](const vector<pair<int, string>>& parts) {
      vector<shared_ptr<Sibling>> v;
      for(const auto& p : parts) {
	v.push_back(std::make_shared<Sibling>(ComboAddress(p.second, 4001)));
      }
      g_siblings.setState(v);
    });


  g_lua.writeFunction("siblingListener", [](const std::string& address) {
      ComboAddress ca(address, 4001);
      
      auto launch = [ca]() {
	thread t1(receiveReports, ca);
	t1.detach();
      };
      if(g_launchWork)
	g_launchWork->push_back(launch);
      else
	launch();
    });

  g_lua.writeFunction("addLocal", [client](const std::string& addr) {
      if(client)
	return;
      try {
	ComboAddress loc(addr, 53);
	g_locals.push_back(loc); /// only works pre-startup, so no sync necessary
      }
      catch(std::exception& e) {
	g_outputBuffer="Error: "+string(e.what())+"\n";
      }
    });
  g_lua.writeFunction("setACL", [](const vector<pair<int, string>>& parts) {
      NetmaskGroup nmg;
      for(const auto& p : parts) {
	nmg.addMask(p.second);
      }
      g_ACL.setState(nmg);
    });
  g_lua.writeFunction("showACL", []() {
      vector<string> vec;

      g_ACL.getCopy().toStringVector(&vec);

      for(const auto& s : vec)
        g_outputBuffer+=s+"\n";

    });
  g_lua.writeFunction("shutdown", []() { _exit(0);} );



  g_lua.writeFunction("webserver", [client](const std::string& address, const std::string& password) {
      if(client)
	return;
      ComboAddress local(address);
      try {
	int sock = socket(local.sin4.sin_family, SOCK_STREAM, 0);
	SSetsockopt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
	SBind(sock, local);
	SListen(sock, 5);
	auto launch=[sock, local, password]() {
	  thread t(dnsdistWebserverThread, sock, local, password);
	  t.detach();
	};
	if(g_launchWork) 
	  g_launchWork->push_back(launch);
	else
	  launch();	    
      }
      catch(std::exception& e) {
	errlog("Unable to bind to webserver socket on %s: %s", local.toStringWithPort(), e.what());
      }

    });
  g_lua.writeFunction("controlSocket", [client](const std::string& str) {
      ComboAddress local(str, 5199);

      if(client) {
	g_serverControl = local;
	return;
      }
      
      try {
	int sock = socket(local.sin4.sin_family, SOCK_STREAM, 0);
	SSetsockopt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
	SBind(sock, local);
	SListen(sock, 5);
	auto launch=[sock, local]() {
	  thread t(controlThread, sock, local);
	  t.detach();
	};
	if(g_launchWork) 
	  g_launchWork->push_back(launch);
	else
	  launch();
	    
      }
      catch(std::exception& e) {
	errlog("Unable to bind to control socket on %s: %s", local.toStringWithPort(), e.what());
      }
    });


  g_lua.writeFunction("report", [](string remote, string login, string pwhash, bool success) {
      LoginTuple lt;
      lt.t=getDoubleTime();
      lt.remote=ComboAddress(remote);
      lt.login=login;
      lt.pwhash=pwhash;
      lt.success=success;
      g_wfdb.reportTuple(lt);
      g_report(&g_wfdb, lt);
    });

  g_lua.writeFunction("stats", []() {
      boost::format fmt("%d reports, %d allow-queries (%d denies), %d entries in database\n");
      g_outputBuffer = (fmt % g_stats.reports % g_stats.allows % g_stats.denieds % g_wfdb.size()).str();

    });
  g_lua.writeFunction("clearDB", []() { 
      g_wfdb.clear();
    });
  g_lua.writeFunction("clearLogin", [](const std::string& login) { 
      auto count=g_wfdb.clearLogin(login);
      g_outputBuffer = "Removed " + std::to_string(count)+" tuples\n";
    });
  g_lua.writeFunction("clearHost", [](const std::string& host) { 
      auto count=g_wfdb.clearRemote(ComboAddress(host));
      g_outputBuffer = "Removed " + std::to_string(count)+" tuples\n";
    });


  g_lua.writeFunction("showLogin", [](const std::string& login) { 
      auto tuples=g_wfdb.getTuplesLogin(login);
      boost::format fmt("%15s %20s %10s %1s\n");
      for(const auto& t : tuples) {
	g_outputBuffer += (fmt % humanTime(t.t) % t.remote.toString() % t.pwhash % (t.success ? "success" : "failure")).str();
      }

    });

  g_lua.writeFunction("showHost", [](const std::string& remote) { 
      auto tuples=g_wfdb.getTuplesRemote(ComboAddress(remote));
      boost::format fmt("%15s %20s %10s %1s\n");
      for(const auto& t : tuples) {
	g_outputBuffer += (fmt % humanTime(t.t) % t.login % t.pwhash % (t.success ? "success" : "failure")).str();
      }

    });

  g_lua.writeFunction("siblings", []() {
      auto siblings = g_siblings.getCopy();
      boost::format fmt("%-35s %-9d %-9d    %s\n");
      g_outputBuffer= (fmt % "Address" % "Sucesses" % "Failures" % "Note").str();
      for(const auto& s : siblings)
	g_outputBuffer += (fmt % s->rem.toStringWithPort() % s->success % s->failures % (s->d_ignoreself ? "Self" : "") ).str();

    });


  g_lua.writeFunction("allow", [](string remote, string login, string pwhash) {
      LoginTuple lt;
      lt.remote=ComboAddress(remote);
      lt.login=login;
      lt.pwhash=pwhash;
      lt.success=0;
      return g_allow(&g_wfdb, lt); // no locking needed, we are in Lua here already!
    });


  g_lua.writeFunction("countFailures", [](ComboAddress remote, int seconds) {
      return g_wfdb.countFailures(remote, seconds);
    });
  g_lua.writeFunction("countDiffFailures", [](ComboAddress remote, int seconds) {
      return g_wfdb.countDiffFailures(remote, seconds);
    });

#ifdef HAVE_GEOIP
  g_lua.writeFunction("initGeoIPDB", []() {
      g_wfgeodb.initGeoIPDB();
    });

  g_lua.writeFunction("lookupCountry", [](ComboAddress address) {
      return g_wfgeodb.lookupCountry(address);
    });
#endif // HAVE_GEOIP

#ifdef HAVE_GETDNS
  g_lua.writeFunction("newDNSResolver", []() { return WFResolver(); });
  g_lua.registerFunction("addResolver", &WFResolver::add_resolver);
  g_lua.registerFunction("setRequestTimeout", &WFResolver::set_request_timeout);
  g_lua.registerFunction("lookupAddrByName", &WFResolver::lookup_address_by_name);
  g_lua.registerFunction("lookupNameByAddr", &WFResolver::lookup_name_by_address);
  g_lua.registerFunction("lookupRBL", &WFResolver::lookupRBL);
  // The following "show.." function are mainly for regression tests
  g_lua.writeFunction("showAddrByName", [](WFResolver resolv, string name) {
      std::vector<std::string> retvec = resolv.lookup_address_by_name(name, 1);
      boost::format fmt("%s %s\n");
      for (const auto s : retvec) {
	g_outputBuffer += (fmt % name % s).str();
      }
    });
  g_lua.writeFunction("showNameByAddr", [](WFResolver resolv, ComboAddress address) {
      std::vector<std::string> retvec = resolv.lookup_name_by_address(address, 1);
      boost::format fmt("%s %s\n");
      for (const auto s : retvec) {
  	g_outputBuffer += (fmt % address.toString() % s).str();
      }
    });
    g_lua.writeFunction("showRBL", [](WFResolver resolv, ComboAddress address, string rblname) {
	std::vector<std::string> retvec = resolv.lookupRBL(address, rblname, 1);
	boost::format fmt("%s\n");
	for (const auto s : retvec) {
	  g_outputBuffer += (fmt % s).str();
	}	
      });	
#endif // HAVE_GETDNS

  g_lua.writeFunction("newStringStatsDB", [](int window_size, int num_windows, const std::vector<pair<std::string, std::string>>& fmvec) {
      return TWStringStatsDBWrapper(window_size, num_windows, fmvec);
    });

  g_lua.registerFunction("twAdd", &TWStringStatsDBWrapper::add);
  g_lua.registerFunction("twSub", &TWStringStatsDBWrapper::sub);
  g_lua.registerFunction("twGet", &TWStringStatsDBWrapper::get);
  g_lua.registerFunction("twGetCurrent", &TWStringStatsDBWrapper::get_current);
  g_lua.registerFunction("twGetWindows", &TWStringStatsDBWrapper::get_windows);
  g_lua.registerFunction("twSetv4Prefix", &TWStringStatsDBWrapper::setv4Prefix);
  g_lua.registerFunction("twSetv6Prefix", &TWStringStatsDBWrapper::setv6Prefix);
  g_lua.registerFunction("twGetSize", &TWStringStatsDBWrapper::get_size);
  g_lua.registerFunction("twSetMaxSize", &TWStringStatsDBWrapper::set_size_soft);

  g_lua.registerMember("t", &LoginTuple::t);
  g_lua.registerMember("remote", &LoginTuple::remote);
  g_lua.registerMember("login", &LoginTuple::login);
  g_lua.registerMember("pwhash", &LoginTuple::pwhash);
  g_lua.registerMember("success", &LoginTuple::success);
  g_lua.registerMember("attrs", &LoginTuple::attrs);
  g_lua.registerMember("attrs_mv", &LoginTuple::attrs_mv);
  g_lua.writeVariable("wfdb", &g_wfdb);
  g_lua.registerFunction("report", &WForceDB::reportTuple);
  g_lua.registerFunction("getTuples", &WForceDB::getTuples);

  g_lua.registerFunction("tostring", &ComboAddress::toString);
  g_lua.writeFunction("newCA", [](string address) { return ComboAddress(address); } );
  g_lua.registerFunction("countDiffFailuresAddress", static_cast<int (WForceDB::*)(const ComboAddress&,  int) const>(&WForceDB::countDiffFailures));
  g_lua.registerFunction("countDiffFailuresAddressLogin", static_cast<int (WForceDB::*)(const ComboAddress&, string, int) const>(&WForceDB::countDiffFailures));

  g_lua.writeFunction("newNetmaskGroup", []() { return NetmaskGroup(); } );

  g_lua.registerFunction("addMask", &NetmaskGroup::addMask);
  g_lua.registerFunction("match", static_cast<bool(NetmaskGroup::*)(const ComboAddress&) const>(&NetmaskGroup::match));

  g_lua.writeFunction("setAllow", [](allow_t func) { g_allow=func;});
  g_lua.writeFunction("setReport", [](report_t func) { g_report=func;});

  g_lua.writeFunction("makeKey", []() {
      g_outputBuffer="setKey("+newKey()+")\n";
    });
  
  g_lua.writeFunction("setKey", [](const std::string& key) {
      if(B64Decode(key, g_key) < 0) {
	g_outputBuffer=string("Unable to decode ")+key+" as Base64";
	errlog("%s", g_outputBuffer);
      }
    });


  g_lua.writeFunction("testCrypto", [](string testmsg)
		      {
			try {
			  SodiumNonce sn, sn2;
			  sn.init();
			  sn2=sn;
			  string encrypted = sodEncryptSym(testmsg, g_key, sn);
			  string decrypted = sodDecryptSym(encrypted, g_key, sn2);
       
			  sn.increment();
			  sn2.increment();

			  encrypted = sodEncryptSym(testmsg, g_key, sn);
			  decrypted = sodDecryptSym(encrypted, g_key, sn2);

			  if(testmsg == decrypted)
			    g_outputBuffer="Everything is ok!\n";
			  else
			    g_outputBuffer="Crypto failed..\n";
       
			}
			catch(...) {
			  g_outputBuffer="Crypto failed..\n";
			}});

  
  std::ifstream ifs(config);
  if(!ifs) 
    warnlog("Unable to read configuration from '%s'", config);
  else
    infolog("Read configuration from '%s'", config);

  g_lua.executeCode(ifs);
  auto ret=*g_launchWork;
  delete g_launchWork;
  g_launchWork=0;
  return ret;
}
