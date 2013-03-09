//
// Carries out the RPC.
//

#include <openssl/md5.h>

#include <boost/foreach.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include "Pathfinder.h"
#include "Log.h"
#include "NetworkOPs.h"
#include "RPCHandler.h"
#include "RPCSub.h"
#include "Application.h"
#include "AccountItems.h"
#include "Wallet.h"
#include "RippleAddress.h"
#include "RippleCalc.h"
#include "RPCErr.h"
#include "AccountState.h"
#include "NicknameState.h"
#include "InstanceCounter.h"
#include "Offer.h"

SETUP_LOG();

static const int rpcCOST_DEFAULT 	= 10;
static const int rpcCOST_EXCEPTION	= 20;
static const int rpcCOST_EXPENSIVE	= 50;

int iAdminGet(const Json::Value& jvRequest, const std::string& strRemoteIp)
{
	int		iRole;
	bool	bPasswordSupplied	= jvRequest.isMember("admin_user") || jvRequest.isMember("admin_password");
	bool	bPasswordRequired	= !theConfig.RPC_ADMIN_USER.empty() || !theConfig.RPC_ADMIN_PASSWORD.empty();

	bool	bPasswordWrong		= bPasswordSupplied
									? bPasswordRequired
										// Supplied, required, and incorrect.
										? theConfig.RPC_ADMIN_USER != (jvRequest.isMember("admin_user") ? jvRequest["admin_user"].asString() : "")
											|| theConfig.RPC_ADMIN_PASSWORD != (jvRequest.isMember("admin_user") ? jvRequest["admin_password"].asString() : "")
										// Supplied and not required.
										: true
									: false;
	// Meets IP restriction for admin.
	bool	bAdminIP			= false;

	BOOST_FOREACH(const std::string& strAllowIp, theConfig.RPC_ADMIN_ALLOW)
	{
		if (strAllowIp == strRemoteIp)
			bAdminIP	= true;
	}

	if (bPasswordWrong							// Wrong
		|| (bPasswordSupplied && !bAdminIP))	// Supplied and doesn't meet IP filter.
	{
		iRole	= RPCHandler::FORBID;
	}
	// If supplied, password is correct.
	else
	{
		// Allow admin, if from admin IP and no password is required or it was supplied and correct.
		iRole = bAdminIP && (!bPasswordRequired || bPasswordSupplied) ? RPCHandler::ADMIN : RPCHandler::GUEST;
	}

	return iRole;
}

RPCHandler::RPCHandler(NetworkOPs* netOps) : mNetOps(netOps)
{ ; }

RPCHandler::RPCHandler(NetworkOPs* netOps, InfoSub::pointer infoSub) : mNetOps(netOps), mInfoSub(infoSub)
{ ; }

Json::Value RPCHandler::transactionSign(Json::Value jvRequest, bool bSubmit)
{
	Json::Value		jvResult;
	RippleAddress	naSeed;
	RippleAddress	raSrcAddressID;

	cLog(lsDEBUG) << boost::str(boost::format("transactionSign: %s") % jvRequest);

	if (!jvRequest.isMember("secret") || !jvRequest.isMember("tx_json"))
	{
		return rpcError(rpcINVALID_PARAMS);
	}

	Json::Value		txJSON		= jvRequest["tx_json"];

	if (!txJSON.isObject())
	{
		return rpcError(rpcINVALID_PARAMS);
	}
	if (!naSeed.setSeedGeneric(jvRequest["secret"].asString()))
	{
		return rpcError(rpcBAD_SEED);
	}
	if (!txJSON.isMember("Account"))
	{
		return rpcError(rpcSRC_ACT_MISSING);
	}
	if (!raSrcAddressID.setAccountID(txJSON["Account"].asString()))
	{
		return rpcError(rpcSRC_ACT_MALFORMED);
	}
	if (!txJSON.isMember("TransactionType"))
	{
		return rpcError(rpcINVALID_PARAMS);
	}

	AccountState::pointer asSrc	= mNetOps->getAccountState(mNetOps->getCurrentLedger(), raSrcAddressID);
	if (!asSrc)
	{
		cLog(lsDEBUG) << boost::str(boost::format("transactionSign: Failed to find source account in current ledger: %s")
			% raSrcAddressID.humanAccountID());

		return rpcError(rpcSRC_ACT_NOT_FOUND);
	}

	if ("Payment" == txJSON["TransactionType"].asString())
	{

		RippleAddress dstAccountID;

		if (!txJSON.isMember("Destination"))
		{
			return rpcError(rpcDST_ACT_MISSING);
		}
		if (!dstAccountID.setAccountID(txJSON["Destination"].asString()))
		{
			return rpcError(rpcDST_ACT_MALFORMED);
		}

		if (!txJSON.isMember("Fee"))
		{
			txJSON["Fee"] = (int) theConfig.FEE_DEFAULT;
		}

		if (txJSON.isMember("Paths") && jvRequest.isMember("build_path"))
		{
			// Asking to build a path when providing one is an error.
			return rpcError(rpcINVALID_PARAMS);
		}

		if (!txJSON.isMember("Paths") && txJSON.isMember("Amount") && jvRequest.isMember("build_path"))
		{
			// Need a ripple path.
			STPathSet	spsPaths;
			uint160		uSrcCurrencyID;
			uint160		uSrcIssuerID;

			STAmount	saSendMax;
			STAmount	saSend;

			if (!txJSON.isMember("Amount")					// Amount required.
				|| !saSend.bSetJson(txJSON["Amount"]))		// Must be valid.
				return rpcError(rpcDST_AMT_MALFORMED);

			if (txJSON.isMember("SendMax"))
			{
				if (!saSendMax.bSetJson(txJSON["SendMax"]))
					return rpcError(rpcINVALID_PARAMS);
			}
			else
			{
				// If no SendMax, default to Amount with sender as issuer.
				saSendMax		= saSend;
				saSendMax.setIssuer(raSrcAddressID.getAccountID());
			}

			if (saSendMax.isNative() && saSend.isNative())
			{
				// Asking to build a path for XRP to XRP is an error.
				return rpcError(rpcINVALID_PARAMS);
			}

			Ledger::pointer lSnapshot = boost::make_shared<Ledger>(
				boost::ref(*mNetOps->getCurrentLedger()), false);
			{
				ScopedUnlock su(theApp->getMasterLock());
				Pathfinder pf(lSnapshot, raSrcAddressID, dstAccountID,
					saSendMax.getCurrency(), saSendMax.getIssuer(), saSend);

				if (!pf.findPaths(theConfig.PATH_SEARCH_SIZE, 3, spsPaths))
				{
					cLog(lsDEBUG) << "transactionSign: build_path: No paths found.";

					return rpcError(rpcNO_PATH);
				}
				else
				{
					cLog(lsDEBUG) << "transactionSign: build_path: " << spsPaths.getJson(0);
				}

				if (!spsPaths.isEmpty())
				{
					txJSON["Paths"]=spsPaths.getJson(0);
				}
			}
		}
	}

	if (!txJSON.isMember("Fee")
		&& (
			"AccountSet" == txJSON["TransactionType"].asString()
			|| "OfferCreate" == txJSON["TransactionType"].asString()
			|| "OfferCancel" == txJSON["TransactionType"].asString()
			|| "TrustSet" == txJSON["TransactionType"].asString()))
	{
		txJSON["Fee"] = (int) theConfig.FEE_DEFAULT;
	}

	if (!txJSON.isMember("Sequence")) txJSON["Sequence"] = asSrc->getSeq();
	if (!txJSON.isMember("Flags")) txJSON["Flags"] = 0;

	Ledger::pointer	lpCurrent		= mNetOps->getCurrentLedger();
	SLE::pointer	sleAccountRoot	= mNetOps->getSLEi(lpCurrent, Ledger::getAccountRootIndex(raSrcAddressID.getAccountID()));

	if (!sleAccountRoot)
	{
		// XXX Ignore transactions for accounts not created.
		return rpcError(rpcSRC_ACT_NOT_FOUND);
	}

	bool			bHaveAuthKey	= false;
	RippleAddress	naAuthorizedPublic;

	RippleAddress	naSecret			= RippleAddress::createSeedGeneric(jvRequest["secret"].asString());
	RippleAddress	naMasterGenerator	= RippleAddress::createGeneratorPublic(naSecret);

	// Find the index of Account from the master generator, so we can generate the public and private keys.
	RippleAddress		naMasterAccountPublic;
	unsigned int		iIndex	= 0;
	bool				bFound	= false;

	// Don't look at ledger entries to determine if the account exists.  Don't want to leak to thin server that these accounts are
	// related.
	while (!bFound && iIndex != theConfig.ACCOUNT_PROBE_MAX)
	{
		naMasterAccountPublic.setAccountPublic(naMasterGenerator, iIndex);

		cLog(lsWARNING) << "authorize: " << iIndex << " : " << naMasterAccountPublic.humanAccountID() << " : " << raSrcAddressID.humanAccountID();

		bFound	= raSrcAddressID.getAccountID() == naMasterAccountPublic.getAccountID();
		if (!bFound)
			++iIndex;
	}

	if (!bFound)
	{
		return rpcError(rpcBAD_SECRET);
	}

	// Use the generator to determine the associated public and private keys.
	RippleAddress	naGenerator			= RippleAddress::createGeneratorPublic(naSecret);
	RippleAddress	naAccountPublic		= RippleAddress::createAccountPublic(naGenerator, iIndex);
	RippleAddress	naAccountPrivate	= RippleAddress::createAccountPrivate(naGenerator, naSecret, iIndex);

	if (bHaveAuthKey
		// The generated pair must match authorized...
		&& naAuthorizedPublic.getAccountID() != naAccountPublic.getAccountID()
		// ... or the master key must have been used.
		&& raSrcAddressID.getAccountID() != naAccountPublic.getAccountID())
	{
		// std::cerr << "iIndex: " << iIndex << std::endl;
		// std::cerr << "sfAuthorizedKey: " << strHex(asSrc->getAuthorizedKey().getAccountID()) << std::endl;
		// std::cerr << "naAccountPublic: " << strHex(naAccountPublic.getAccountID()) << std::endl;

		return rpcError(rpcSRC_ACT_NOT_FOUND);
	}

	std::auto_ptr<STObject>	sopTrans;

	try
	{
		sopTrans = STObject::parseJson(txJSON);
	}
	catch (std::exception& e)
	{
		jvResult["error"]			= "malformedTransaction";
		jvResult["error_exception"]	= e.what();

		return jvResult;
	}

	sopTrans->setFieldVL(sfSigningPubKey, naAccountPublic.getAccountPublic());

	SerializedTransaction::pointer stpTrans;

	try
	{
		stpTrans = boost::make_shared<SerializedTransaction>(*sopTrans);
	}
	catch (std::exception& e)
	{
		jvResult["error"]			= "invalidTransaction";
		jvResult["error_exception"]	= e.what();

		return jvResult;
	}

	if (jvRequest.isMember("debug_signing")) {
		jvResult["tx_unsigned"]		= strHex(stpTrans->getSerializer().peekData());
		jvResult["tx_signing_hash"]	= stpTrans->getSigningHash().ToString();
	}

	// FIXME: For performance, transactions should not be signed in this code path.
	stpTrans->sign(naAccountPrivate);

	Transaction::pointer			tpTrans;

	try
	{
		tpTrans		= boost::make_shared<Transaction>(stpTrans, false);
	}
	catch (std::exception& e)
	{
		jvResult["error"]			= "internalTransaction";
		jvResult["error_exception"]	= e.what();

		return jvResult;
	}

	try
	{
		tpTrans	= mNetOps->submitTransactionSync(tpTrans, bSubmit); // FIXME: For performance, should use asynch interface

		if (!tpTrans) {
			jvResult["error"]			= "invalidTransaction";
			jvResult["error_exception"]	= "Unable to sterilize transaction.";

			return jvResult;
		}
	}
	catch (std::exception& e)
	{
		jvResult["error"]			= "internalSubmit";
		jvResult["error_exception"]	= e.what();

		return jvResult;
	}

	try
	{
		jvResult["tx_json"]		= tpTrans->getJson(0);
		jvResult["tx_blob"]		= strHex(tpTrans->getSTransaction()->getSerializer().peekData());

		if (temUNCERTAIN != tpTrans->getResult())
		{
			std::string	sToken;
			std::string	sHuman;

			transResultInfo(tpTrans->getResult(), sToken, sHuman);

			jvResult["engine_result"]			= sToken;
			jvResult["engine_result_code"]		= tpTrans->getResult();
			jvResult["engine_result_message"]	= sHuman;
		}
		return jvResult;
	}
	catch (std::exception& e)
	{
		jvResult["error"]			= "internalJson";
		jvResult["error_exception"]	= e.what();

		return jvResult;
	}
}

// Look up the master public generator for a regular seed so we may index source accounts ids.
// --> naRegularSeed
// <-- naMasterGenerator
Json::Value RPCHandler::getMasterGenerator(Ledger::ref lrLedger, const RippleAddress& naRegularSeed, RippleAddress& naMasterGenerator)
{
	RippleAddress		na0Public;		// To find the generator's index.
	RippleAddress		na0Private;		// To decrypt the master generator's cipher.
	RippleAddress		naGenerator	= RippleAddress::createGeneratorPublic(naRegularSeed);

	na0Public.setAccountPublic(naGenerator, 0);
	na0Private.setAccountPrivate(naGenerator, naRegularSeed, 0);

	SLE::pointer		sleGen			= mNetOps->getGenerator(lrLedger, na0Public.getAccountID());

	if (!sleGen)
	{
		// No account has been claimed or has had it password set for seed.
		return rpcError(rpcNO_ACCOUNT);
	}

	std::vector<unsigned char>	vucCipher			= sleGen->getFieldVL(sfGenerator);
	std::vector<unsigned char>	vucMasterGenerator	= na0Private.accountPrivateDecrypt(na0Public, vucCipher);
	if (vucMasterGenerator.empty())
	{
		return rpcError(rpcFAIL_GEN_DECRPYT);
	}

	naMasterGenerator.setGenerator(vucMasterGenerator);

	return Json::Value(Json::objectValue);
}

// Given a seed and a source account get the regular public and private key for authorizing transactions.
// - Make sure the source account can pay.
// --> naRegularSeed : To find the generator
// --> naSrcAccountID : Account we want the public and private regular keys to.
// <-- naAccountPublic : Regular public key for naSrcAccountID
// <-- naAccountPrivate : Regular private key for naSrcAccountID
// <-- saSrcBalance: Balance minus fee.
// --> naVerifyGenerator : If provided, the found master public generator must match.
// XXX Be more lenient, allow use of master generator on claimed accounts.
Json::Value RPCHandler::authorize(Ledger::ref lrLedger,
	const RippleAddress& naRegularSeed, const RippleAddress& naSrcAccountID,
	RippleAddress& naAccountPublic, RippleAddress& naAccountPrivate,
	STAmount& saSrcBalance, const STAmount& saFee, AccountState::pointer& asSrc,
	const RippleAddress& naVerifyGenerator)
{
	// Source/paying account must exist.
	asSrc	= mNetOps->getAccountState(lrLedger, naSrcAccountID);
	if (!asSrc)
	{
		return rpcError(rpcSRC_ACT_NOT_FOUND);
	}

	RippleAddress	naMasterGenerator;

	if (asSrc->bHaveAuthorizedKey())
	{
		Json::Value	obj	= getMasterGenerator(lrLedger, naRegularSeed, naMasterGenerator);

		if (!obj.empty())
			return obj;
	}
	else
	{
		// Try the seed as a master seed.
		naMasterGenerator	= RippleAddress::createGeneratorPublic(naRegularSeed);
	}

	// If naVerifyGenerator is provided, make sure it is the master generator.
	if (naVerifyGenerator.isValid() && naMasterGenerator != naVerifyGenerator)
	{
		return rpcError(rpcWRONG_SEED);
	}

	// Find the index of the account from the master generator, so we can generate the public and private keys.
	RippleAddress		naMasterAccountPublic;
	unsigned int		iIndex	= 0;
	bool				bFound	= false;

	// Don't look at ledger entries to determine if the account exists.  Don't want to leak to thin server that these accounts are
	// related.
	while (!bFound && iIndex != theConfig.ACCOUNT_PROBE_MAX)
	{
		naMasterAccountPublic.setAccountPublic(naMasterGenerator, iIndex);

		cLog(lsDEBUG) << "authorize: " << iIndex << " : " << naMasterAccountPublic.humanAccountID() << " : " << naSrcAccountID.humanAccountID();

		bFound	= naSrcAccountID.getAccountID() == naMasterAccountPublic.getAccountID();
		if (!bFound)
			++iIndex;
	}

	if (!bFound)
	{
		return rpcError(rpcACT_NOT_FOUND);
	}

	// Use the regular generator to determine the associated public and private keys.
	RippleAddress		naGenerator	= RippleAddress::createGeneratorPublic(naRegularSeed);

	naAccountPublic.setAccountPublic(naGenerator, iIndex);
	naAccountPrivate.setAccountPrivate(naGenerator, naRegularSeed, iIndex);

	if (asSrc->bHaveAuthorizedKey() && (asSrc->getAuthorizedKey().getAccountID() != naAccountPublic.getAccountID()))
	{
		// std::cerr << "iIndex: " << iIndex << std::endl;
		// std::cerr << "sfAuthorizedKey: " << strHex(asSrc->getAuthorizedKey().getAccountID()) << std::endl;
		// std::cerr << "naAccountPublic: " << strHex(naAccountPublic.getAccountID()) << std::endl;

		return rpcError(rpcPASSWD_CHANGED);
	}

	saSrcBalance	= asSrc->getBalance();

	if (saSrcBalance < saFee)
	{
		cLog(lsINFO) << "authorize: Insufficient funds for fees: fee=" << saFee.getText() << " balance=" << saSrcBalance.getText();

		return rpcError(rpcINSUF_FUNDS);
	}
	else
	{
		saSrcBalance -= saFee;
	}

	return Json::Value();
}

// --> strIdent: public key, account ID, or regular seed.
// --> bStrict: Only allow account id or public key.
// <-- bIndex: true if iIndex > 0 and used the index.
Json::Value RPCHandler::accountFromString(Ledger::ref lrLedger, RippleAddress& naAccount, bool& bIndex, const std::string& strIdent, const int iIndex, const bool bStrict)
{
	RippleAddress	naSeed;

	if (naAccount.setAccountPublic(strIdent) || naAccount.setAccountID(strIdent))
	{
		// Got the account.
		bIndex	= false;
	}
	else if (bStrict)
	{
		return rpcError(rpcACT_MALFORMED);
	}
	// Must be a seed.
	else if (!naSeed.setSeedGeneric(strIdent))
	{
		return rpcError(rpcBAD_SEED);
	}
	else
	{
		// We allow the use of the seeds to access #0.
		// This is poor practice and merely for debuging convenience.
		RippleAddress		naRegular0Public;
		RippleAddress		naRegular0Private;

		RippleAddress		naGenerator		= RippleAddress::createGeneratorPublic(naSeed);

		naRegular0Public.setAccountPublic(naGenerator, 0);
		naRegular0Private.setAccountPrivate(naGenerator, naSeed, 0);

		//		uint160				uGeneratorID	= naRegular0Public.getAccountID();
		SLE::pointer		sleGen			= mNetOps->getGenerator(lrLedger, naRegular0Public.getAccountID());
		if (!sleGen)
		{
			// Didn't find a generator map, assume it is a master generator.
			nothing();
		}
		else
		{
			// Found master public key.
			std::vector<unsigned char>	vucCipher				= sleGen->getFieldVL(sfGenerator);
			std::vector<unsigned char>	vucMasterGenerator		= naRegular0Private.accountPrivateDecrypt(naRegular0Public, vucCipher);
			if (vucMasterGenerator.empty())
			{
				rpcError(rpcNO_GEN_DECRPYT);
			}

			naGenerator.setGenerator(vucMasterGenerator);
		}

		bIndex	= !iIndex;

		naAccount.setAccountPublic(naGenerator, iIndex);
	}

	return Json::Value(Json::objectValue);
}

// {
//   account: <indent>,
//   account_index : <index> // optional
//   strict: <bool>					// true, only allow public keys and addresses. false, default.
//   ledger_hash : <ledger>
//   ledger_index : <ledger_index>
// }
Json::Value RPCHandler::doAccountInfo(Json::Value jvRequest, int& cost)
{
	Ledger::pointer		lpLedger;
	Json::Value			jvResult	= lookupLedger(jvRequest, lpLedger);

	if (!lpLedger)
		return jvResult;

	if (!jvRequest.isMember("account") && !jvRequest.isMember("ident"))
		return rpcError(rpcINVALID_PARAMS);

	std::string		strIdent	= jvRequest.isMember("account") ? jvRequest["account"].asString() : jvRequest["ident"].asString();
	bool			bIndex;
	int				iIndex		= jvRequest.isMember("account_index") ? jvRequest["account_index"].asUInt() : 0;
	bool			bStrict		= jvRequest.isMember("strict") && jvRequest["strict"].asBool();
	RippleAddress	naAccount;

	// Get info on account.

	Json::Value		jvAccepted		= accountFromString(lpLedger, naAccount, bIndex, strIdent, iIndex, bStrict);

	if (!jvAccepted.empty())
		return jvAccepted;

	AccountState::pointer asAccepted	= mNetOps->getAccountState(lpLedger, naAccount);

	if (asAccepted)
	{
		asAccepted->addJson(jvAccepted);

		jvResult["account_data"]	= jvAccepted;
	}
	else
	{
		jvResult	= rpcError(rpcACT_NOT_FOUND);
	}

	return jvResult;
}

// {
//   ip: <string>,
//   port: <number>
// }
// XXX Might allow domain for manual connections.
Json::Value RPCHandler::doConnect(Json::Value jvRequest, int& cost)
{
	if (theConfig.RUN_STANDALONE)
		return "cannot connect in standalone mode";

	if (!jvRequest.isMember("ip"))
		return rpcError(rpcINVALID_PARAMS);

	std::string strIp	= jvRequest["ip"].asString();
	int			iPort	= jvRequest.isMember("port") ? jvRequest["port"].asInt() : -1;

	// XXX Validate legal IP and port
	theApp->getConnectionPool().connectTo(strIp, iPort);

	return "connecting";
}

#if ENABLE_INSECURE
// {
//   key: <string>
// }
Json::Value RPCHandler::doDataDelete(Json::Value jvRequest, int& cost)
{
	if (!jvRequest.isMember("key"))
		return rpcError(rpcINVALID_PARAMS);

	std::string	strKey = jvRequest["key"].asString();

	Json::Value	ret = Json::Value(Json::objectValue);

	if (theApp->getWallet().dataDelete(strKey))
	{
		ret["key"]		= strKey;
	}
	else
	{
		ret	= rpcError(rpcINTERNAL);
	}

	return ret;
}
#endif

#if ENABLE_INSECURE
// {
//   key: <string>
// }
Json::Value RPCHandler::doDataFetch(Json::Value jvRequest, int& cost)
{
	if (!jvRequest.isMember("key"))
		return rpcError(rpcINVALID_PARAMS);

	std::string	strKey = jvRequest["key"].asString();
	std::string	strValue;

	Json::Value	ret = Json::Value(Json::objectValue);

	ret["key"]		= strKey;
	if (theApp->getWallet().dataFetch(strKey, strValue))
		ret["value"]	= strValue;

	return ret;
}
#endif

#if ENABLE_INSECURE
// {
//   key: <string>
//   value: <string>
// }
Json::Value RPCHandler::doDataStore(Json::Value jvRequest, int& cost)
{
	if (!jvRequest.isMember("key")
		|| !jvRequest.isMember("value"))
		return rpcError(rpcINVALID_PARAMS);

	std::string	strKey		= jvRequest["key"].asString();
	std::string	strValue	= jvRequest["value"].asString();

	Json::Value	ret = Json::Value(Json::objectValue);

	if (theApp->getWallet().dataStore(strKey, strValue))
	{
		ret["key"]		= strKey;
		ret["value"]	= strValue;
	}
	else
	{
		ret	= rpcError(rpcINTERNAL);
	}

	return ret;
}
#endif

#if 0
// XXX Needs to be revised for new paradigm
// nickname_info <nickname>
// Note: Nicknames are not automatically looked up by commands as they are advisory and can be changed.
Json::Value RPCHandler::doNicknameInfo(Json::Value params)
{
	std::string	strNickname	= params[0u].asString();
	boost::trim(strNickname);

	if (strNickname.empty())
	{
		return rpcError(rpcNICKNAME_MALFORMED);
	}

	NicknameState::pointer	nsSrc	= mNetOps->getNicknameState(uint256(0), strNickname);
	if (!nsSrc)
	{
		return rpcError(rpcNICKNAME_MISSING);
	}

	Json::Value ret(Json::objectValue);

	ret["nickname"]	= strNickname;

	nsSrc->addJson(ret);

	return ret;
}
#endif

// {
//   'ident' : <indent>,
//   'account_index' : <index> // optional
// }
// XXX This would be better if it took the ledger.
Json::Value RPCHandler::doOwnerInfo(Json::Value jvRequest, int& cost)
{
	if (!jvRequest.isMember("account") && !jvRequest.isMember("ident"))
		return rpcError(rpcINVALID_PARAMS);

	std::string		strIdent	= jvRequest.isMember("account") ? jvRequest["account"].asString() : jvRequest["ident"].asString();
	bool			bIndex;
	int				iIndex		= jvRequest.isMember("account_index") ? jvRequest["account_index"].asUInt() : 0;
	RippleAddress	raAccount;

	Json::Value		ret;

	// Get info on account.

	Json::Value		jAccepted	= accountFromString(mNetOps->getClosedLedger(), raAccount, bIndex, strIdent, iIndex, false);

	ret["accepted"]	= jAccepted.empty() ? mNetOps->getOwnerInfo(mNetOps->getClosedLedger(), raAccount) : jAccepted;

	Json::Value		jCurrent	= accountFromString(mNetOps->getCurrentLedger(), raAccount, bIndex, strIdent, iIndex, false);

	ret["current"]	= jCurrent.empty() ? mNetOps->getOwnerInfo(mNetOps->getCurrentLedger(), raAccount) : jCurrent;

	return ret;
}

Json::Value RPCHandler::doPeers(Json::Value, int& cost)
{
	Json::Value jvResult(Json::objectValue);

	jvResult["peers"]	= theApp->getConnectionPool().getPeersJson();

	return jvResult;
}

Json::Value RPCHandler::doPing(Json::Value, int& cost)
{
	return Json::Value(Json::objectValue);
}

// profile offers <pass_a> <account_a> <currency_offer_a> <account_b> <currency_offer_b> <count> [submit]
// profile 0:offers 1:pass_a 2:account_a 3:currency_offer_a 4:account_b 5:currency_offer_b 6:<count> 7:[submit]
// issuer is the offering account
// --> submit: 'submit|true|false': defaults to false
// Prior to running allow each to have a credit line of what they will be getting from the other account.
Json::Value RPCHandler::doProfile(Json::Value jvRequest, int& cost)
{
	/* need to fix now that sharedOfferCreate is gone
	int				iArgs	= jvRequest.size();
	RippleAddress	naSeedA;
	RippleAddress	naAccountA;
	uint160			uCurrencyOfferA;
	RippleAddress	naSeedB;
	RippleAddress	naAccountB;
	uint160			uCurrencyOfferB;
	uint32			iCount	= 100;
	bool			bSubmit	= false;

	if (iArgs < 6 || "offers" != jvRequest[0u].asString())
	{
		return rpcError(rpcINVALID_PARAMS);
	}

	if (!naSeedA.setSeedGeneric(jvRequest[1u].asString()))							// <pass_a>
		return rpcError(rpcINVALID_PARAMS);

	naAccountA.setAccountID(jvRequest[2u].asString());								// <account_a>

	if (!STAmount::currencyFromString(uCurrencyOfferA, jvRequest[3u].asString()))	// <currency_offer_a>
		return rpcError(rpcINVALID_PARAMS);

	naAccountB.setAccountID(jvRequest[4u].asString());								// <account_b>
	if (!STAmount::currencyFromString(uCurrencyOfferB, jvRequest[5u].asString()))	// <currency_offer_b>
		return rpcError(rpcINVALID_PARAMS);

	iCount	= lexical_cast_s<uint32>(jvRequest[6u].asString());

	if (iArgs >= 8 && "false" != jvRequest[7u].asString())
		bSubmit	= true;

	Log::setMinSeverity(lsFATAL,true);

	boost::posix_time::ptime			ptStart(boost::posix_time::microsec_clock::local_time());

	for(unsigned int n=0; n<iCount; n++)
	{
		RippleAddress			naMasterGeneratorA;
		RippleAddress			naAccountPublicA;
		RippleAddress			naAccountPrivateA;
		AccountState::pointer	asSrcA;
		STAmount				saSrcBalanceA;

		Json::Value				jvObjA		= authorize(uint256(0), naSeedA, naAccountA, naAccountPublicA, naAccountPrivateA,
			saSrcBalanceA, theConfig.FEE_DEFAULT, asSrcA, naMasterGeneratorA);

		if (!jvObjA.empty())
			return jvObjA;

		Transaction::pointer	tpOfferA	= Transaction::sharedOfferCreate(
			naAccountPublicA, naAccountPrivateA,
			naAccountA,													// naSourceAccount,
			asSrcA->getSeq(),											// uSeq
			theConfig.FEE_DEFAULT,
			0,															// uSourceTag,
			false,														// bPassive
			STAmount(uCurrencyOfferA, naAccountA.getAccountID(), 1),	// saTakerPays
			STAmount(uCurrencyOfferB, naAccountB.getAccountID(), 1+n),	// saTakerGets
			0);															// uExpiration

		if (bSubmit)
			tpOfferA	= mNetOps->submitTransactionSync(tpOfferA); // FIXME: Don't use synch interface
	}

	boost::posix_time::ptime			ptEnd(boost::posix_time::microsec_clock::local_time());
	boost::posix_time::time_duration	tdInterval		= ptEnd-ptStart;
	long								lMicroseconds	= tdInterval.total_microseconds();
	int									iTransactions	= iCount;
	float								fRate			= lMicroseconds ? iTransactions/(lMicroseconds/1000000.0) : 0.0;

	Json::Value obj(Json::objectValue);

	obj["transactions"]		= iTransactions;
	obj["submit"]			= bSubmit;
	obj["start"]			= boost::posix_time::to_simple_string(ptStart);
	obj["end"]				= boost::posix_time::to_simple_string(ptEnd);
	obj["interval"]			= boost::posix_time::to_simple_string(tdInterval);
	obj["rate_per_second"]	= fRate;
	*/
	Json::Value obj(Json::objectValue);
	return obj;
}

// {
//   account: <account>|<nickname>|<account_public_key>
//   account_index: <number>		// optional, defaults to 0.
//   ledger_hash : <ledger>
//   ledger_index : <ledger_index>
// }
Json::Value RPCHandler::doAccountLines(Json::Value jvRequest, int& cost)
{
	Ledger::pointer		lpLedger;
	Json::Value			jvResult	= lookupLedger(jvRequest, lpLedger);

	if (!lpLedger)
		return jvResult;

	if (!jvRequest.isMember("account"))
		return rpcError(rpcINVALID_PARAMS);

	std::string		strIdent	= jvRequest["account"].asString();
	bool			bIndex		= jvRequest.isMember("account_index");
	int				iIndex		= bIndex ? jvRequest["account_index"].asUInt() : 0;

	RippleAddress	raAccount;

	jvResult	= accountFromString(lpLedger, raAccount, bIndex, strIdent, iIndex, false);

	if (!jvResult.empty())
		return jvResult;

	// Get info on account.

	jvResult["account"]	= raAccount.humanAccountID();
	if (bIndex)
		jvResult["account_index"]	= iIndex;

	AccountState::pointer	as		= mNetOps->getAccountState(lpLedger, raAccount);
	if (as)
	{
		Json::Value	jsonLines(Json::arrayValue);

		jvResult["account"]	= raAccount.humanAccountID();


		// XXX This is wrong, we do access the current ledger and do need to worry about changes.
		// We access a committed ledger and need not worry about changes.

		AccountItems rippleLines(raAccount.getAccountID(), lpLedger, AccountItem::pointer(new RippleState()));

		BOOST_FOREACH(AccountItem::ref item, rippleLines.getItems())
		{
			RippleState* line=(RippleState*)item.get();

			STAmount		saBalance	= line->getBalance();
			STAmount		saLimit		= line->getLimit();
			STAmount		saLimitPeer	= line->getLimitPeer();

			Json::Value			jPeer	= Json::Value(Json::objectValue);

			jPeer["account"]		= RippleAddress::createHumanAccountID(line->getAccountIDPeer());
			// Amount reported is positive if current account holds other account's IOUs.
			// Amount reported is negative if other account holds current account's IOUs.
			jPeer["balance"]		= saBalance.getText();
			jPeer["currency"]		= saBalance.getHumanCurrency();
			jPeer["limit"]			= saLimit.getText();
			jPeer["limit_peer"]		= saLimitPeer.getText();
			jPeer["quality_in"]		= static_cast<Json::UInt>(line->getQualityIn());
			jPeer["quality_out"]	= static_cast<Json::UInt>(line->getQualityOut());

			jsonLines.append(jPeer);
		}
		jvResult["lines"]	= jsonLines;
	}
	else
	{
		jvResult	= rpcError(rpcACT_NOT_FOUND);
	}

	return jvResult;
}

// {
//   account: <account>|<nickname>|<account_public_key>
//   account_index: <number>		// optional, defaults to 0.
//   ledger_hash : <ledger>
//   ledger_index : <ledger_index>
// }
Json::Value RPCHandler::doAccountOffers(Json::Value jvRequest, int& cost)
{
	Ledger::pointer		lpLedger;
	Json::Value			jvResult	= lookupLedger(jvRequest, lpLedger);

	if (!lpLedger)
		return jvResult;

	if (!jvRequest.isMember("account"))
		return rpcError(rpcINVALID_PARAMS);

	std::string		strIdent	= jvRequest["account"].asString();
	bool			bIndex		= jvRequest.isMember("account_index");
	int				iIndex		= bIndex ? jvRequest["account_index"].asUInt() : 0;

	RippleAddress	raAccount;

	jvResult	= accountFromString(lpLedger, raAccount, bIndex, strIdent, iIndex, false);

	if (!jvResult.empty())
		return jvResult;

	// Get info on account.

	jvResult["account"]	= raAccount.humanAccountID();
	if (bIndex)
		jvResult["account_index"]	= iIndex;

	AccountState::pointer	as		= mNetOps->getAccountState(lpLedger, raAccount);
	if (as)
	{
		Json::Value	jsonLines(Json::arrayValue);

		AccountItems offers(raAccount.getAccountID(), lpLedger, AccountItem::pointer(new Offer()));
		BOOST_FOREACH(AccountItem::ref item, offers.getItems())
		{
			Offer* offer=(Offer*)item.get();

			STAmount takerPays	= offer->getTakerPays();
			STAmount takerGets	= offer->getTakerGets();
			//RippleAddress account	= offer->getAccount();

			Json::Value	obj	= Json::Value(Json::objectValue);

			//obj["account"]		= account.humanAccountID();
			obj["taker_pays"]		= takerPays.getJson(0);
			obj["taker_gets"]		= takerGets.getJson(0);
			obj["seq"]				= offer->getSeq();

			jsonLines.append(obj);
		}
		jvResult["offers"]	= jsonLines;
	}
	else
	{
		jvResult	= rpcError(rpcACT_NOT_FOUND);
	}

	return jvResult;
}

// {
//   "ledger_hash" : ledger,             // Optional.
//   "ledger_index" : ledger_index,      // Optional.
//   "taker_gets" : { "currency": currency, "issuer" : address },
//   "taker_pays" : { "currency": currency, "issuer" : address },
//   "taker" : address,					 // Optional.
//   "marker" : element,                 // Optional.
//   "limit" : integer,                  // Optional.
//   "proof" : boolean                   // Defaults to false.
// }
Json::Value RPCHandler::doBookOffers(Json::Value jvRequest, int& cost)
{
	if (theApp->getJobQueue().getJobCountGE(jtCLIENT) > 200)
	{
		return rpcError(rpcTOO_BUSY);
	}

	Ledger::pointer		lpLedger;
	Json::Value			jvResult	= lookupLedger(jvRequest, lpLedger);

	if (!lpLedger)
		return jvResult;

	if (!jvRequest.isMember("taker_pays") || !jvRequest.isMember("taker_gets"))
		return rpcError(rpcINVALID_PARAMS);

	uint160		uTakerPaysCurrencyID;
	uint160		uTakerPaysIssuerID;
	Json::Value	jvTakerPays	= jvRequest["taker_pays"];

	// Parse mandatory currency.
	if (!jvTakerPays.isMember("currency")
		|| !STAmount::currencyFromString(uTakerPaysCurrencyID, jvTakerPays["currency"].asString()))
	{
		cLog(lsINFO) << "Bad taker_pays currency.";

		return rpcError(rpcSRC_CUR_MALFORMED);
	}
	// Parse optional issuer.
	else if (((jvTakerPays.isMember("issuer"))
			&& (!jvTakerPays["issuer"].isString()
				|| !STAmount::issuerFromString(uTakerPaysIssuerID, jvTakerPays["issuer"].asString())))
		// Don't allow illegal issuers.
		|| (!uTakerPaysCurrencyID != !uTakerPaysIssuerID)
		|| ACCOUNT_ONE == uTakerPaysIssuerID)
	{
		cLog(lsINFO) << "Bad taker_pays issuer.";

		return rpcError(rpcSRC_ISR_MALFORMED);
	}

	uint160		uTakerGetsCurrencyID;
	uint160		uTakerGetsIssuerID;
	Json::Value	jvTakerGets	= jvRequest["taker_gets"];

	// Parse mandatory currency.
	if (!jvTakerGets.isMember("currency")
		|| !STAmount::currencyFromString(uTakerGetsCurrencyID, jvTakerGets["currency"].asString()))
	{
		cLog(lsINFO) << "Bad taker_pays currency.";

		return rpcError(rpcSRC_CUR_MALFORMED);
	}
	// Parse optional issuer.
	else if (((jvTakerGets.isMember("issuer"))
			&& (!jvTakerGets["issuer"].isString()
				|| !STAmount::issuerFromString(uTakerGetsIssuerID, jvTakerGets["issuer"].asString())))
		// Don't allow illegal issuers.
		|| (!uTakerGetsCurrencyID != !uTakerGetsIssuerID)
		|| ACCOUNT_ONE == uTakerGetsIssuerID)
	{
		cLog(lsINFO) << "Bad taker_gets issuer.";

		return rpcError(rpcDST_ISR_MALFORMED);
	}

	if (uTakerPaysCurrencyID == uTakerGetsCurrencyID
		&& uTakerPaysIssuerID == uTakerGetsIssuerID) {
		cLog(lsINFO) << "taker_gets same as taker_pays.";

		return rpcError(rpcBAD_MARKET);
	}

	RippleAddress	raTakerID;

	if (!jvRequest.isMember("taker"))
	{
		raTakerID.setAccountID(ACCOUNT_ONE);
	}
	else if (!raTakerID.setAccountID(jvRequest["taker"].asString()))
	{
		return rpcError(rpcBAD_ISSUER);
	}

	const bool			bProof		= jvRequest.isMember("proof");
	const unsigned int	iLimit		= jvRequest.isMember("limit") ? jvRequest["limit"].asUInt() : 0;
	const Json::Value	jvMarker	= jvRequest.isMember("marker") ? jvRequest["marker"] : Json::Value(Json::nullValue);

	mNetOps->getBookPage(lpLedger, uTakerPaysCurrencyID, uTakerPaysIssuerID, uTakerGetsCurrencyID, uTakerGetsIssuerID, raTakerID.getAccountID(), bProof, iLimit, jvMarker, jvResult);

	return jvResult;
}

// Result:
// {
//   random: <uint256>
// }
Json::Value RPCHandler::doRandom(Json::Value jvRequest, int& cost)
{
	uint256			uRandom;

	try
	{
		getRand(uRandom.begin(), uRandom.size());

		Json::Value jvResult;

		jvResult["random"]	= uRandom.ToString();

		return jvResult;
	}
	catch (...)
	{
		return rpcError(rpcINTERNAL);
	}
}

// TODO:
// - Add support for specifying non-endpoint issuer.
// - Return fully expanded path with proof.
//   - Allows clients to verify path exists.
// - Return canonicalized path.
//   - From a trusted server, allows clients to use path without manipulation.
Json::Value RPCHandler::doRipplePathFind(Json::Value jvRequest, int& cost)
{
	int jc = theApp->getJobQueue().getJobCountGE(jtCLIENT);
	if (jc > 200)
	{
		cLog(lsDEBUG) << "Too busy for RPF: " << jc;
		return rpcError(rpcTOO_BUSY);
	}

	RippleAddress	raSrc;
	RippleAddress	raDst;
	STAmount		saDstAmount;
	Ledger::pointer	lpLedger;
	Json::Value		jvResult	= lookupLedger(jvRequest, lpLedger);

	if (!lpLedger)
		return jvResult;

	if (!jvRequest.isMember("source_account"))
	{
		jvResult	= rpcError(rpcSRC_ACT_MISSING);
	}
	else if (!jvRequest["source_account"].isString()
				|| !raSrc.setAccountID(jvRequest["source_account"].asString()))
	{
		jvResult	= rpcError(rpcSRC_ACT_MALFORMED);
	}
	else if (!jvRequest.isMember("destination_account"))
	{
		jvResult	= rpcError(rpcDST_ACT_MISSING);
	}
	else if (!jvRequest["destination_account"].isString()
				|| !raDst.setAccountID(jvRequest["destination_account"].asString()))
	{
		jvResult	= rpcError(rpcDST_ACT_MALFORMED);
	}
	else if (
		// Parse saDstAmount.
		!jvRequest.isMember("destination_amount")
		|| !saDstAmount.bSetJson(jvRequest["destination_amount"])
		|| (!!saDstAmount.getCurrency() && (!saDstAmount.getIssuer() || ACCOUNT_ONE == saDstAmount.getIssuer())))
	{
		cLog(lsINFO) << "Bad destination_amount.";
		jvResult	= rpcError(rpcINVALID_PARAMS);
	}
	else if (
		// Checks on source_currencies.
		jvRequest.isMember("source_currencies")
		&& (!jvRequest["source_currencies"].isArray()
			|| !jvRequest["source_currencies"].size())	// Don't allow empty currencies.
		)
	{
		cLog(lsINFO) << "Bad source_currencies.";
		jvResult	= rpcError(rpcINVALID_PARAMS);
	}
	else
	{
		Json::Value		jvSrcCurrencies;

		if (jvRequest.isMember("source_currencies"))
		{
			jvSrcCurrencies	= jvRequest["source_currencies"];
		}
		else
		{
			boost::unordered_set<uint160>	usCurrencies	= usAccountSourceCurrencies(raSrc, lpLedger);

			// Add XRP as a source currency.
			// YYY Only bother if they are above reserve.
			usCurrencies.insert(uint160(CURRENCY_XRP));

			jvSrcCurrencies				= Json::Value(Json::arrayValue);

			BOOST_FOREACH(const uint160& uCurrency, usCurrencies)
			{
				Json::Value	jvCurrency(Json::objectValue);

				jvCurrency["currency"]	= STAmount::createHumanCurrency(uCurrency);

				jvSrcCurrencies.append(jvCurrency);
			}
		}

		cost = rpcCOST_EXPENSIVE;
		Ledger::pointer lSnapShot = boost::make_shared<Ledger>(boost::ref(*lpLedger), false);
		LedgerEntrySet lesSnapshot(lSnapShot);

		ScopedUnlock	su(theApp->getMasterLock()); // As long as we have a locked copy of the ledger, we can unlock.

		Json::Value	jvArray(Json::arrayValue);

		for (unsigned int i=0; i != jvSrcCurrencies.size(); ++i) {
			Json::Value	jvSource		= jvSrcCurrencies[i];
			uint160		uSrcCurrencyID;
			uint160		uSrcIssuerID;

			// Parse mandatory currency.
			if (!jvSource.isMember("currency")
				|| !STAmount::currencyFromString(uSrcCurrencyID, jvSource["currency"].asString()))
			{
				cLog(lsINFO) << "Bad currency.";

				return rpcError(rpcSRC_CUR_MALFORMED);
			}
			if (uSrcCurrencyID.isNonZero())
				uSrcIssuerID = raSrc.getAccountID();

			// Parse optional issuer.
			if (jvSource.isMember("issuer") &&
					((!jvSource["issuer"].isString() ||
					!STAmount::issuerFromString(uSrcIssuerID, jvSource["issuer"].asString())) ||
					(uSrcIssuerID.isZero() != uSrcCurrencyID.isZero()) ||
					(ACCOUNT_ONE == uSrcIssuerID)))
			{
				cLog(lsINFO) << "Bad issuer.";

				return rpcError(rpcSRC_ISR_MALFORMED);
			}

			STPathSet	spsComputed;
			Pathfinder	pf(lSnapShot, raSrc, raDst, uSrcCurrencyID, uSrcIssuerID, saDstAmount);

			if (!pf.findPaths(theConfig.PATH_SEARCH_SIZE, 3, spsComputed))
			{
				cLog(lsDEBUG) << "ripple_path_find: No paths found.";
			}
			else
			{
				std::vector<PathState::pointer>	vpsExpanded;
				STAmount						saMaxAmountAct;
				STAmount						saDstAmountAct;
				STAmount						saMaxAmount(
													uSrcCurrencyID,
													!!uSrcIssuerID
														? uSrcIssuerID		// Use specifed issuer.
														: !!uSrcCurrencyID	// Default to source account.
															? raSrc.getAccountID()
															: ACCOUNT_XRP,
													1);
					saMaxAmount.negate();

				TER	terResult	=
					RippleCalc::rippleCalc(
						lesSnapshot,
						saMaxAmountAct,			// <--
						saDstAmountAct,			// <--
						vpsExpanded,			// <--
						saMaxAmount,			// --> Amount to send is unlimited to get an estimate.
						saDstAmount,			// --> Amount to deliver.
						raDst.getAccountID(),	// --> Account to deliver to.
						raSrc.getAccountID(),	// --> Account sending from.
						spsComputed,			// --> Path set.
						false,					// --> Don't allow partial payment. This is for normal fill or kill payments.
												// Must achieve delivery goal.
						false,					// --> Don't limit quality. Average quality is wanted for normal payments.
						false,					// --> Allow direct ripple to be added to path set. to path set.
						true);					// --> Stand alone mode, no point in deleting unfundeds.

				// cLog(lsDEBUG) << "ripple_path_find: PATHS IN: " << spsComputed.size() << " : " << spsComputed.getJson(0);
				// cLog(lsDEBUG) << "ripple_path_find: PATHS EXP: " << vpsExpanded.size();


				cLog(lsDEBUG)
					<< boost::str(boost::format("ripple_path_find: saMaxAmount=%s saDstAmount=%s saMaxAmountAct=%s saDstAmountAct=%s")
						% saMaxAmount
						% saDstAmount
						% saMaxAmountAct
						% saDstAmountAct);

				if (tesSUCCESS == terResult)
				{
					Json::Value	jvEntry(Json::objectValue);

					STPathSet	spsCanonical;

					// Reuse the expanded as it would need to be calcuated anyway to produce the canonical.
					// (At least unless we make a direct canonical.)
					RippleCalc::setCanonical(spsCanonical, vpsExpanded, false);

					jvEntry["source_amount"]	= saMaxAmountAct.getJson(0);
//					jvEntry["paths_expanded"]	= vpsExpanded.getJson(0);
					jvEntry["paths_canonical"]	= spsCanonical.getJson(0);
					jvEntry["paths_computed"]	= spsComputed.getJson(0);

					jvArray.append(jvEntry);
				}
				else
				{
					std::string	strToken;
					std::string	strHuman;

					transResultInfo(terResult, strToken, strHuman);

					cLog(lsDEBUG)
						<< boost::str(boost::format("ripple_path_find: %s %s %s")
							% strToken
							% strHuman
							% spsComputed.getJson(0));
				}
			}
		}

		// Each alternative differs by source currency.
		jvResult["alternatives"] = jvArray;
	}

	cLog(lsDEBUG)
		<< boost::str(boost::format("ripple_path_find< %s")
			% jvResult);

	return jvResult;
}

// {
//   tx_json: <object>,
//   secret: <secret>
// }
Json::Value RPCHandler::doSign(Json::Value jvRequest, int& cost)
{
	cost = rpcCOST_EXPENSIVE;
	return transactionSign(jvRequest, false);
}

// {
//   tx_json: <object>,
//   secret: <secret>
// }
Json::Value RPCHandler::doSubmit(Json::Value jvRequest, int& cost)
{
	if (!jvRequest.isMember("tx_blob"))
	{
		return transactionSign(jvRequest, true);
	}

	Json::Value					jvResult;

	std::vector<unsigned char>	vucBlob(strUnHex(jvRequest["tx_blob"].asString()));

	if (!vucBlob.size())
	{
		return rpcError(rpcINVALID_PARAMS);
	}
	cost = rpcCOST_EXPENSIVE;

	Serializer					sTrans(vucBlob);
	SerializerIterator			sitTrans(sTrans);

	SerializedTransaction::pointer stpTrans;

	try
	{
		stpTrans = boost::make_shared<SerializedTransaction>(boost::ref(sitTrans));
	}
	catch (std::exception& e)
	{
		jvResult["error"]			= "invalidTransaction";
		jvResult["error_exception"]	= e.what();

		return jvResult;
	}

	Transaction::pointer			tpTrans;

	try
	{
		tpTrans		= boost::make_shared<Transaction>(stpTrans, false);
	}
	catch (std::exception& e)
	{
		jvResult["error"]			= "internalTransaction";
		jvResult["error_exception"]	= e.what();

		return jvResult;
	}

	try
	{
		(void) mNetOps->processTransaction(tpTrans);
	}
	catch (std::exception& e)
	{
		jvResult["error"]			= "internalSubmit";
		jvResult["error_exception"]	= e.what();

		return jvResult;
	}

	try
	{
		jvResult["tx_json"]		= tpTrans->getJson(0);
		jvResult["tx_blob"]		= strHex(tpTrans->getSTransaction()->getSerializer().peekData());

		if (temUNCERTAIN != tpTrans->getResult())
		{
			std::string	sToken;
			std::string	sHuman;

			transResultInfo(tpTrans->getResult(), sToken, sHuman);

			jvResult["engine_result"]			= sToken;
			jvResult["engine_result_code"]		= tpTrans->getResult();
			jvResult["engine_result_message"]	= sHuman;
		}
		return jvResult;
	}
	catch (std::exception& e)
	{
		jvResult["error"]			= "internalJson";
		jvResult["error_exception"]	= e.what();

		return jvResult;
	}
}

Json::Value RPCHandler::doConsensusInfo(Json::Value, int& cost)
{
	Json::Value ret(Json::objectValue);

	ret["info"] = mNetOps->getConsensusInfo();

	return ret;
}

Json::Value RPCHandler::doServerInfo(Json::Value, int& cost)
{
	Json::Value ret(Json::objectValue);

	ret["info"]	= mNetOps->getServerInfo(true, mRole == ADMIN);

	return ret;
}

Json::Value RPCHandler::doServerState(Json::Value, int& cost)
{
	Json::Value ret(Json::objectValue);

	ret["state"]	= mNetOps->getServerInfo(false, mRole == ADMIN);

	return ret;
}

// {
//   start: <index>
// }
Json::Value RPCHandler::doTxHistory(Json::Value jvRequest, int& cost)
{
	if (!jvRequest.isMember("start"))
		return rpcError(rpcINVALID_PARAMS);

	unsigned int startIndex = jvRequest["start"].asUInt();
	Json::Value	obj;
	Json::Value	txs;

	obj["index"] = startIndex;

	std::string sql =
		boost::str(boost::format("SELECT * FROM Transactions ORDER BY LedgerSeq desc LIMIT %u,20")
		% startIndex);

	{
		Database* db = theApp->getTxnDB()->getDB();
		ScopedLock sl (theApp->getTxnDB()->getDBLock());

		SQL_FOREACH(db, sql)
		{
			Transaction::pointer trans=Transaction::transactionFromSQL(db, false);
			if (trans) txs.append(trans->getJson(0));
		}
	}

	obj["txs"]=txs;

	return obj;
}

// {
//   transaction: <hex>
// }
Json::Value RPCHandler::doTx(Json::Value jvRequest, int& cost)
{
	if (!jvRequest.isMember("transaction"))
		return rpcError(rpcINVALID_PARAMS);

	bool binary = jvRequest.isMember("binary") && jvRequest["binary"].asBool();

	std::string strTransaction	= jvRequest["transaction"].asString();

	if (Transaction::isHexTxID(strTransaction))
	{ // transaction by ID
		uint256 txid(strTransaction);

		Transaction::pointer txn = theApp->getMasterTransaction().fetch(txid, true);

		if (!txn)
			return rpcError(rpcTXN_NOT_FOUND);

#ifdef READY_FOR_NEW_TX_FORMAT
		Json::Value ret;
		ret["transaction"] = txn->getJson(0, binary);
#else
		Json::Value ret = txn->getJson(0, binary);
#endif

		if (txn->getLedger() != 0)
		{
			Ledger::pointer lgr = mNetOps->getLedgerBySeq(txn->getLedger());
			if (lgr)
			{
				bool okay = false;
				if (binary)
				{
					std::string meta;
					if (lgr->getMetaHex(txid, meta))
					{
						ret["meta"] = meta;
						okay = true;
					}
				}
				else
				{
					TransactionMetaSet::pointer set;
					if (lgr->getTransactionMeta(txid, set))
					{
						okay = true;
						ret["meta"] = set->getJson(0);
					}
				}
				if (okay)
					ret["validated"] = mNetOps->isValidated(lgr);
			}
		}

		return ret;
	}

	return rpcError(rpcNOT_IMPL);
}

Json::Value RPCHandler::doLedgerClosed(Json::Value, int& cost)
{
	Json::Value jvResult;

	uint256	uLedger	= mNetOps->getClosedLedgerHash();

	jvResult["ledger_index"]		= mNetOps->getLedgerID(uLedger);
	jvResult["ledger_hash"]			= uLedger.ToString();
	//jvResult["ledger_time"]		= uLedger.

	return jvResult;
}

Json::Value RPCHandler::doLedgerCurrent(Json::Value, int& cost)
{
	Json::Value jvResult;

	jvResult["ledger_current_index"]	= mNetOps->getCurrentLedgerID();

	return jvResult;
}

// ledger [id|index|current|closed] [full]
// {
//    ledger: 'current' | 'closed' | <uint256> | <number>,	// optional
//    full: true | false	// optional, defaults to false.
// }
Json::Value RPCHandler::doLedger(Json::Value jvRequest, int& cost)
{
	if (!jvRequest.isMember("ledger"))
	{
		Json::Value ret(Json::objectValue), current(Json::objectValue), closed(Json::objectValue);

		theApp->getLedgerMaster().getCurrentLedger()->addJson(current, 0);
		theApp->getLedgerMaster().getClosedLedger()->addJson(closed, 0);

		ret["open"] = current;
		ret["closed"] = closed;

		return ret;
	}

	std::string		strLedger	= jvRequest["ledger"].asString();
	Ledger::pointer ledger;

	if (strLedger == "current")
		ledger = theApp->getLedgerMaster().getCurrentLedger();
	else if (strLedger == "closed")
		ledger = theApp->getLedgerMaster().getClosedLedger();
	else if (strLedger.size() > 12)
		ledger = theApp->getLedgerMaster().getLedgerByHash(uint256(strLedger));
	else
		ledger = theApp->getOPs().getLedgerBySeq(jvRequest["ledger"].asUInt());

	if (!ledger)
		return rpcError(rpcLGR_NOT_FOUND);

	bool full = jvRequest.isMember("full") && jvRequest["full"].asBool();

	Json::Value ret(Json::objectValue);

	ScopedUnlock(theApp->getMasterLock());
	ledger->addJson(ret, full ? LEDGER_JSON_FULL : 0);

	return ret;
}

// { account: <account>, ledger: <integer> }
// { account: <account>, ledger_min: <integer>, ledger_max: <integer> }
// THIS ROUTINE DOESN'T SCALE.
// FIXME: Require admin.
// FIXME: Doesn't report database holes.
// FIXME: For consistency change inputs to: ledger_index, ledger_index_min, ledger_index_max.
Json::Value RPCHandler::doAccountTransactions(Json::Value jvRequest, int& cost)
{
	RippleAddress	raAccount;
	uint32			minLedger;
	uint32			maxLedger;

	if (!jvRequest.isMember("account"))
		return rpcError(rpcINVALID_PARAMS);

	if (!raAccount.setAccountID(jvRequest["account"].asString()))
		return rpcError(rpcACT_MALFORMED);

	if (jvRequest.isMember("ledger_min") && jvRequest.isMember("ledger_max"))
	{
		minLedger	= jvRequest["ledger_min"].asUInt();
		maxLedger	= jvRequest["ledger_max"].asUInt();
	}
	else
	{
		Ledger::pointer l;
		Json::Value ret = lookupLedger(jvRequest, l);
		if (!l)
			return ret;
		minLedger = maxLedger = l->getLedgerSeq();
	}

	if ((maxLedger < minLedger) || (maxLedger == 0))
	{
		return rpcError(rpcLGR_IDXS_INVALID);
	}

#ifndef DEBUG
	try
	{
#endif
		unsigned int vl = mNetOps->getValidatedSeq();
		ScopedUnlock su(theApp->getMasterLock());

		Json::Value ret(Json::objectValue);
		ret["account"] = raAccount.humanAccountID();

		if (jvRequest.isMember("binary") && jvRequest["binary"].asBool())
		{
			std::vector<NetworkOPs::txnMetaLedgerType> txns =
				mNetOps->getAccountTxsB(raAccount, minLedger, maxLedger);
			for (std::vector<NetworkOPs::txnMetaLedgerType>::const_iterator it = txns.begin(), end = txns.end();
				it != end; ++it)
			{
				Json::Value obj(Json::objectValue);
				obj["transaction"] = it->get<0>();
				obj["meta"] = it->get<1>();
				obj["inLedger"] = it->get<2>();
				if (it->get<2>() > vl)
					obj["validated"] = false;
				else if (mNetOps->haveLedger(it->get<2>()))
					obj["validated"] = true;
				ret["transactions"].append(obj);
			}
		}
		else
		{
			std::vector< std::pair<Transaction::pointer, TransactionMetaSet::pointer> > txns = mNetOps->getAccountTxs(raAccount, minLedger, maxLedger);
			for (std::vector< std::pair<Transaction::pointer, TransactionMetaSet::pointer> >::iterator it = txns.begin(), end = txns.end(); it != end; ++it)
			{
				Json::Value	obj(Json::objectValue);

				if (it->first)
					obj["tx"] = it->first->getJson(1);
				if (it->second)
				{
					obj["meta"] = it->second->getJson(0);

					uint32 s = it->second->getLgrSeq();
					if (s > vl)
						obj["validated"] = false;
					else if (mNetOps->haveLedger(s))
						obj["validated"] = true;
				}

				ret["transactions"].append(obj);
			}
		}
		return ret;
#ifndef DEBUG
	}
	catch (...)
	{
		return rpcError(rpcINTERNAL);
	}
#endif
}

// {
//   secret: <string>	// optional
// }
//
// This command requires admin access because it makes no sense to ask an untrusted server for this.
Json::Value RPCHandler::doValidationCreate(Json::Value jvRequest, int& cost) {
	RippleAddress	raSeed;
	Json::Value		obj(Json::objectValue);

	if (!jvRequest.isMember("secret"))
	{
		cLog(lsDEBUG) << "Creating random validation seed.";

		raSeed.setSeedRandom();					// Get a random seed.
	}
	else if (!raSeed.setSeedGeneric(jvRequest["secret"].asString()))
	{
		return rpcError(rpcBAD_SEED);
	}

	obj["validation_public_key"]	= RippleAddress::createNodePublic(raSeed).humanNodePublic();
	obj["validation_seed"]			= raSeed.humanSeed();
	obj["validation_key"]			= raSeed.humanSeed1751();

	return obj;
}

// {
//   secret: <string>
// }
Json::Value RPCHandler::doValidationSeed(Json::Value jvRequest, int& cost) {
	Json::Value obj(Json::objectValue);

	if (!jvRequest.isMember("secret"))
	{
		std::cerr << "Unset validation seed." << std::endl;

		theConfig.VALIDATION_SEED.clear();
		theConfig.VALIDATION_PUB.clear();
		theConfig.VALIDATION_PRIV.clear();
	}
	else if (!theConfig.VALIDATION_SEED.setSeedGeneric(jvRequest["secret"].asString()))
	{
		theConfig.VALIDATION_PUB.clear();
		theConfig.VALIDATION_PRIV.clear();

		return rpcError(rpcBAD_SEED);
	}
	else
	{
		theConfig.VALIDATION_PUB = RippleAddress::createNodePublic(theConfig.VALIDATION_SEED);
		theConfig.VALIDATION_PRIV = RippleAddress::createNodePrivate(theConfig.VALIDATION_SEED);

		obj["validation_public_key"]	= theConfig.VALIDATION_PUB.humanNodePublic();
		obj["validation_seed"]			= theConfig.VALIDATION_SEED.humanSeed();
		obj["validation_key"]			= theConfig.VALIDATION_SEED.humanSeed1751();
	}

	return obj;
}

Json::Value RPCHandler::accounts(Ledger::ref lrLedger, const RippleAddress& naMasterGenerator)
{
	Json::Value jsonAccounts(Json::arrayValue);

	// YYY Don't want to leak to thin server that these accounts are related.
	// YYY Would be best to alternate requests to servers and to cache results.
	unsigned int	uIndex	= 0;

	do {
		RippleAddress		naAccount;

		naAccount.setAccountPublic(naMasterGenerator, uIndex++);

		AccountState::pointer as	= mNetOps->getAccountState(lrLedger, naAccount);
		if (as)
		{
			Json::Value	jsonAccount(Json::objectValue);

			as->addJson(jsonAccount);

			jsonAccounts.append(jsonAccount);
		}
		else
		{
			uIndex	= 0;
		}
	} while (uIndex);

	return jsonAccounts;
}

// {
//   seed: <string>
//   ledger_hash : <ledger>
//   ledger_index : <ledger_index>
// }
Json::Value RPCHandler::doWalletAccounts(Json::Value jvRequest, int& cost)
{
	Ledger::pointer		lpLedger;
	Json::Value			jvResult	= lookupLedger(jvRequest, lpLedger);

	if (!lpLedger)
		return jvResult;

	RippleAddress	naSeed;

	if (!jvRequest.isMember("seed") || !naSeed.setSeedGeneric(jvRequest["seed"].asString()))
	{
		return rpcError(rpcBAD_SEED);
	}

	// Try the seed as a master seed.
	RippleAddress	naMasterGenerator	= RippleAddress::createGeneratorPublic(naSeed);

	Json::Value jsonAccounts	= accounts(lpLedger, naMasterGenerator);

	if (jsonAccounts.empty())
	{
		// No account via seed as master, try seed a regular.
		Json::Value	ret	= getMasterGenerator(lpLedger, naSeed, naMasterGenerator);

		if (!ret.empty())
			return ret;

		ret["accounts"]	= accounts(lpLedger, naMasterGenerator);

		return ret;
	}
	else
	{
		// Had accounts via seed as master, return them.
		Json::Value ret(Json::objectValue);

		ret["accounts"]	= jsonAccounts;

		return ret;
	}
}

Json::Value RPCHandler::doLogRotate(Json::Value, int& cost)
{
	return Log::rotateLog();
}

// {
//  passphrase: <string>
// }
Json::Value RPCHandler::doWalletPropose(Json::Value jvRequest, int& cost)
{
	RippleAddress	naSeed;
	RippleAddress	naAccount;

	if (jvRequest.isMember("passphrase"))
	{
		naSeed	= RippleAddress::createSeedGeneric(jvRequest["passphrase"].asString());
	}
	else
	{
		naSeed.setSeedRandom();
	}

	RippleAddress	naGenerator	= RippleAddress::createGeneratorPublic(naSeed);
	naAccount.setAccountPublic(naGenerator, 0);

	Json::Value obj(Json::objectValue);

	obj["master_seed"]		= naSeed.humanSeed();
	obj["master_seed_hex"]	= naSeed.getSeed().ToString();
	//obj["master_key"]		= naSeed.humanSeed1751();
	obj["account_id"]		= naAccount.humanAccountID();

	return obj;
}

// {
//   secret: <string>
// }
Json::Value RPCHandler::doWalletSeed(Json::Value jvRequest, int& cost)
{
	RippleAddress	raSeed;
	bool			bSecret	= jvRequest.isMember("secret");

	if (bSecret && !raSeed.setSeedGeneric(jvRequest["secret"].asString()))
	{
		return rpcError(rpcBAD_SEED);
	}
	else
	{
		RippleAddress	raAccount;

		if (!bSecret)
		{
			raSeed.setSeedRandom();
		}

		RippleAddress	raGenerator	= RippleAddress::createGeneratorPublic(raSeed);

		raAccount.setAccountPublic(raGenerator, 0);

		Json::Value obj(Json::objectValue);

		obj["seed"]		= raSeed.humanSeed();
		obj["key"]		= raSeed.humanSeed1751();

		return obj;
	}
}

#if ENABLE_INSECURE
// TODO: for now this simply checks if this is the admin account
// TODO: need to prevent them hammering this over and over
// TODO: maybe a better way is only allow admin from local host
// {
//   username: <string>,
//   password: <string>
// }
Json::Value RPCHandler::doLogin(Json::Value jvRequest, int& cost)
{
	if (!jvRequest.isMember("username")
		|| !jvRequest.isMember("password"))
		return rpcError(rpcINVALID_PARAMS);

	if (jvRequest["username"].asString() == theConfig.RPC_USER && jvRequest["password"].asString() == theConfig.RPC_PASSWORD)
	{
		//mRole=ADMIN;
		return "logged in";
	}
	else
	{
		return "nope";
	}
}
#endif

static void textTime(std::string& text, int& seconds, const char *unitName, int unitVal)
{
	int i = seconds / unitVal;
	if (i == 0)
		return;
	seconds -= unitVal * i;
	if (!text.empty())
		text += ", ";
	text += boost::lexical_cast<std::string>(i);
	text += " ";
	text += unitName;
	if (i > 1)
		text += "s";
}

// {
//   min_count: <number>  // optional, defaults to 10
// }
Json::Value RPCHandler::doGetCounts(Json::Value jvRequest, int& cost)
{
	int minCount = 10;

	if (jvRequest.isMember("min_count"))
		minCount = jvRequest["min_count"].asUInt();

	std::vector<InstanceType::InstanceCount> count = InstanceType::getInstanceCounts(minCount);

	Json::Value ret(Json::objectValue);

	BOOST_FOREACH(InstanceType::InstanceCount& it, count)
		ret[it.first] = it.second;

	int dbKB = theApp->getLedgerDB()->getDB()->getKBUsed();
	if (dbKB > 0)
		ret["dbKB"] = dbKB;

	std::string uptime;
	int s = upTime();
	textTime(uptime, s, "year", 365*24*60*60);
	textTime(uptime, s, "day", 24*60*60);
	textTime(uptime, s, "hour", 60*60);
	textTime(uptime, s, "minute", 60);
	textTime(uptime, s, "second", 1);
	ret["uptime"] = uptime;

	return ret;
}

Json::Value RPCHandler::doLogLevel(Json::Value jvRequest, int& cost)
{
	// log_level
	if (!jvRequest.isMember("severity"))
	{ // get log severities
		Json::Value ret(Json::objectValue);
		Json::Value lev(Json::objectValue);

		lev["base"] = Log::severityToString(Log::getMinSeverity());
		std::vector< std::pair<std::string, std::string> > logTable = LogPartition::getSeverities();
		typedef std::map<std::string, std::string>::value_type stringPair;
		BOOST_FOREACH(const stringPair& it, logTable)
			lev[it.first] = it.second;

		ret["levels"] = lev;
		return ret;
	}

	LogSeverity sv = Log::stringToSeverity(jvRequest["severity"].asString());
	if (sv == lsINVALID)
		return rpcError(rpcINVALID_PARAMS);

	// log_level severity
	if (!jvRequest.isMember("partition"))
	{ // set base log severity
		Log::setMinSeverity(sv, true);
		return Json::objectValue;
	}

	// log_level partition severity base?
	if (jvRequest.isMember("partition"))
	{ // set partition severity
		std::string partition(jvRequest["partition"].asString());
		if (boost::iequals(partition, "base"))
			Log::setMinSeverity(sv,false);
		else if (!LogPartition::setSeverity(partition, sv))
			return rpcError(rpcINVALID_PARAMS);

		return Json::objectValue;
	}

	return rpcError(rpcINVALID_PARAMS);
}

// {
//   node: <domain>|<node_public>,
//   comment: <comment>				// optional
// }
Json::Value RPCHandler::doUnlAdd(Json::Value jvRequest, int& cost)
{
	std::string	strNode		= jvRequest.isMember("node") ? jvRequest["node"].asString() : "";
	std::string	strComment	= jvRequest.isMember("comment") ? jvRequest["comment"].asString() : "";

	RippleAddress	raNodePublic;

	if (raNodePublic.setNodePublic(strNode))
	{
		theApp->getUNL().nodeAddPublic(raNodePublic, UniqueNodeList::vsManual, strComment);

		return "adding node by public key";
	}
	else
	{
		theApp->getUNL().nodeAddDomain(strNode, UniqueNodeList::vsManual, strComment);

		return "adding node by domain";
	}
}

// {
//   node: <domain>|<public_key>
// }
Json::Value RPCHandler::doUnlDelete(Json::Value jvRequest, int& cost)
{
	if (!jvRequest.isMember("node"))
		return rpcError(rpcINVALID_PARAMS);

	std::string	strNode		= jvRequest["node"].asString();

	RippleAddress	raNodePublic;

	if (raNodePublic.setNodePublic(strNode))
	{
		theApp->getUNL().nodeRemovePublic(raNodePublic);

		return "removing node by public key";
	}
	else
	{
		theApp->getUNL().nodeRemoveDomain(strNode);

		return "removing node by domain";
	}
}

Json::Value RPCHandler::doUnlList(Json::Value, int& cost)
{
	Json::Value obj(Json::objectValue);

	obj["unl"]=theApp->getUNL().getUnlJson();

	return obj;
}

// Populate the UNL from a local validators.txt file.
Json::Value RPCHandler::doUnlLoad(Json::Value, int& cost)
{
	if (theConfig.VALIDATORS_FILE.empty() || !theApp->getUNL().nodeLoad(theConfig.VALIDATORS_FILE))
	{
		return rpcError(rpcLOAD_FAILED);
	}

	return "loading";
}


// Populate the UNL from ripple.com's validators.txt file.
Json::Value RPCHandler::doUnlNetwork(Json::Value jvRequest, int& cost)
{
	theApp->getUNL().nodeNetwork();

	return "fetching";
}

// unl_reset
Json::Value RPCHandler::doUnlReset(Json::Value jvRequest, int& cost)
{
	theApp->getUNL().nodeReset();

	return "removing nodes";
}

// unl_score
Json::Value RPCHandler::doUnlScore(Json::Value, int& cost)
{
	theApp->getUNL().nodeScore();

	return "scoring requested";
}

Json::Value RPCHandler::doStop(Json::Value, int& cost)
{
	theApp->stop();

	return SYSTEM_NAME " server stopping";
}

Json::Value RPCHandler::doLedgerAccept(Json::Value, int& cost)
{
	Json::Value jvResult;

	if (!theConfig.RUN_STANDALONE)
	{
		jvResult["error"]	= "notStandAlone";
	}
	else
	{
		mNetOps->acceptLedger();

		jvResult["ledger_current_index"]	= mNetOps->getCurrentLedgerID();
	}

	return jvResult;
}

// {
//   ledger_hash : <ledger>,
//   ledger_index : <ledger_index>
// }
// XXX In this case, not specify either ledger does not mean ledger current. It means any ledger.
Json::Value RPCHandler::doTransactionEntry(Json::Value jvRequest, int& cost)
{
	Ledger::pointer		lpLedger;
	Json::Value			jvResult	= lookupLedger(jvRequest, lpLedger);

	if (!lpLedger)
		return jvResult;

	if (!jvRequest.isMember("tx_hash"))
	{
		jvResult["error"]	= "fieldNotFoundTransaction";
	}
	else if (!jvRequest.isMember("ledger_hash") && !jvRequest.isMember("ledger_index"))
	{
		// We don't work on ledger current.

		jvResult["error"]	= "notYetImplemented";	// XXX We don't support any transaction yet.
	}
	else
	{
		uint256						uTransID;
		// XXX Relying on trusted WSS client. Would be better to have a strict routine, returning success or failure.
		uTransID.SetHex(jvRequest["tx_hash"].asString());

		if (!lpLedger) {
			jvResult["error"]	= "ledgerNotFound";
		}
		else
		{
			Transaction::pointer		tpTrans;
			TransactionMetaSet::pointer	tmTrans;

			if (!lpLedger->getTransaction(uTransID, tpTrans, tmTrans))
			{
				jvResult["error"]	= "transactionNotFound";
			}
			else
			{
				jvResult["tx_json"]		= tpTrans->getJson(0);
				jvResult["metadata"]	= tmTrans->getJson(0);
				// 'accounts'
				// 'engine_...'
				// 'ledger_...'
			}
		}
	}

	return jvResult;
}

Json::Value RPCHandler::lookupLedger(Json::Value jvRequest, Ledger::pointer& lpLedger)
{
	Json::Value jvResult;

	uint256	uLedger			= jvRequest.isMember("ledger_hash") ? uint256(jvRequest["ledger_hash"].asString()) : 0;
	int32	iLedgerIndex	= jvRequest.isMember("ledger_index") && jvRequest["ledger_index"].isNumeric() ? jvRequest["ledger_index"].asInt() : -2;

	if (!!uLedger)
	{
		// Ledger directly specified.
		lpLedger	= mNetOps->getLedgerByHash(uLedger);

		if (!lpLedger)
		{
			jvResult["error"]	= "ledgerNotFound";

			return jvResult;
		}

		iLedgerIndex	= lpLedger->getLedgerSeq();	// Set the current index, override if needed.
	}
	if (-1 == iLedgerIndex)
	{
		lpLedger		= theApp->getLedgerMaster().getClosedLedger();
		iLedgerIndex	= lpLedger->getLedgerSeq();
	}
	if (-2 == iLedgerIndex)
	{
		// Default to current ledger.
		lpLedger		= mNetOps->getCurrentLedger();
		iLedgerIndex	= lpLedger->getLedgerSeq();
	}
	if (-3 == iLedgerIndex)
	{ // Last fully-validated ledger
		lpLedger		= mNetOps->getValidatedLedger();
		iLedgerIndex 	= lpLedger->getLedgerSeq();
	}

	if (iLedgerIndex <= 0)
	{
		jvResult["error"]	= "ledgerNotFound";

		return jvResult;
	}

	if (!lpLedger)
	{
		lpLedger		= mNetOps->getLedgerBySeq(iLedgerIndex);

		if (!lpLedger)
		{
			jvResult["error"]	= "ledgerNotFound";	// ledger_index from future?

			return jvResult;
		}
	}

	if (lpLedger->isClosed())
	{
		if (!!uLedger)
			jvResult["ledger_hash"]			= uLedger.ToString();

		jvResult["ledger_index"]		= iLedgerIndex;
	}
	else
	{
		jvResult["ledger_current_index"]	= iLedgerIndex;
	}

	return jvResult;
}

// {
//   ledger_hash : <ledger>
//   ledger_index : <ledger_index>
//   ...
// }
Json::Value RPCHandler::doLedgerEntry(Json::Value jvRequest, int& cost)
{
	Ledger::pointer		lpLedger;
	Json::Value			jvResult	= lookupLedger(jvRequest, lpLedger);

	if (!lpLedger)
		return jvResult;

	uint256		uNodeIndex;
	bool		bNodeBinary	= false;

	if (jvRequest.isMember("index"))
	{
		// XXX Needs to provide proof.
		uNodeIndex.SetHex(jvRequest["index"].asString());
		bNodeBinary	= true;
	}
	else if (jvRequest.isMember("account_root"))
	{
		RippleAddress	naAccount;

		if (!naAccount.setAccountID(jvRequest["account_root"].asString())
			|| !naAccount.getAccountID())
		{
			jvResult["error"]	= "malformedAddress";
		}
		else
		{
			uNodeIndex = Ledger::getAccountRootIndex(naAccount.getAccountID());
		}
	}
	else if (jvRequest.isMember("directory"))
	{

		if (!jvRequest.isObject())
		{
			uNodeIndex.SetHex(jvRequest["directory"].asString());
		}
		else if (jvRequest["directory"].isMember("sub_index")
			&& !jvRequest["directory"]["sub_index"].isIntegral())
		{
			jvResult["error"]	= "malformedRequest";
		}
		else
		{
			uint64	uSubIndex = jvRequest["directory"].isMember("sub_index")
				? jvRequest["directory"]["sub_index"].asUInt()
				: 0;

			if (jvRequest["directory"].isMember("dir_root"))
			{
				uint256	uDirRoot;

				uDirRoot.SetHex(jvRequest["dir_root"].asString());

				uNodeIndex	= Ledger::getDirNodeIndex(uDirRoot, uSubIndex);
			}
			else if (jvRequest["directory"].isMember("owner"))
			{
				RippleAddress	naOwnerID;

				if (!naOwnerID.setAccountID(jvRequest["directory"]["owner"].asString()))
				{
					jvResult["error"]	= "malformedAddress";
				}
				else
				{
					uint256	uDirRoot	= Ledger::getOwnerDirIndex(naOwnerID.getAccountID());

					uNodeIndex	= Ledger::getDirNodeIndex(uDirRoot, uSubIndex);
				}
			}
			else
			{
				jvResult["error"]	= "malformedRequest";
			}
		}
	}
	else if (jvRequest.isMember("generator"))
	{
		RippleAddress	naGeneratorID;

		if (!jvRequest.isObject())
		{
			uNodeIndex.SetHex(jvRequest["generator"].asString());
		}
		else if (!jvRequest["generator"].isMember("regular_seed"))
		{
			jvResult["error"]	= "malformedRequest";
		}
		else if (!naGeneratorID.setSeedGeneric(jvRequest["generator"]["regular_seed"].asString()))
		{
			jvResult["error"]	= "malformedAddress";
		}
		else
		{
			RippleAddress		na0Public;		// To find the generator's index.
			RippleAddress		naGenerator	= RippleAddress::createGeneratorPublic(naGeneratorID);

			na0Public.setAccountPublic(naGenerator, 0);

			uNodeIndex	= Ledger::getGeneratorIndex(na0Public.getAccountID());
		}
	}
	else if (jvRequest.isMember("offer"))
	{
		RippleAddress	naAccountID;

		if (!jvRequest.isObject())
		{
			uNodeIndex.SetHex(jvRequest["offer"].asString());
		}
		else if (!jvRequest["offer"].isMember("account")
			|| !jvRequest["offer"].isMember("seq")
			|| !jvRequest["offer"]["seq"].isIntegral())
		{
			jvResult["error"]	= "malformedRequest";
		}
		else if (!naAccountID.setAccountID(jvRequest["offer"]["account"].asString()))
		{
			jvResult["error"]	= "malformedAddress";
		}
		else
		{
			uint32		uSequence	= jvRequest["offer"]["seq"].asUInt();

			uNodeIndex	= Ledger::getOfferIndex(naAccountID.getAccountID(), uSequence);
		}
	}
	else if (jvRequest.isMember("ripple_state"))
	{
		RippleAddress	naA;
		RippleAddress	naB;
		uint160			uCurrency;
		Json::Value		jvRippleState	= jvRequest["ripple_state"];

		if (!jvRippleState.isMember("currency")
			|| !jvRippleState.isMember("accounts")
			|| !jvRippleState["accounts"].isArray()
			|| 2 != jvRippleState["accounts"].size()
			|| !jvRippleState["accounts"][0u].isString()
			|| !jvRippleState["accounts"][1u].isString()
			|| jvRippleState["accounts"][0u].asString() == jvRippleState["accounts"][1u].asString()
			) {

				cLog(lsINFO)
					<< boost::str(boost::format("ledger_entry: ripple_state: accounts: %d currency: %d array: %d size: %d equal: %d")
					% jvRippleState.isMember("accounts")
					% jvRippleState.isMember("currency")
					% jvRippleState["accounts"].isArray()
					% jvRippleState["accounts"].size()
					% (jvRippleState["accounts"][0u].asString() == jvRippleState["accounts"][1u].asString())
					);

				jvResult["error"]	= "malformedRequest";
		}
		else if (!naA.setAccountID(jvRippleState["accounts"][0u].asString())
			|| !naB.setAccountID(jvRippleState["accounts"][1u].asString())) {
				jvResult["error"]	= "malformedAddress";
		}
		else if (!STAmount::currencyFromString(uCurrency, jvRippleState["currency"].asString())) {
			jvResult["error"]	= "malformedCurrency";
		}
		else
		{
			uNodeIndex	= Ledger::getRippleStateIndex(naA, naB, uCurrency);
		}
	}
	else
	{
		jvResult["error"]	= "unknownOption";
	}

	if (!!uNodeIndex)
	{
		SLE::pointer	sleNode	= mNetOps->getSLEi(lpLedger, uNodeIndex);

		if (!sleNode)
		{
			// Not found.
			// XXX Should also provide proof.
			jvResult["error"]		= "entryNotFound";
		}
		else if (bNodeBinary)
		{
			// XXX Should also provide proof.
			Serializer s;

			sleNode->add(s);

			jvResult["node_binary"]	= strHex(s.peekData());
			jvResult["index"]		= uNodeIndex.ToString();
		}
		else
		{
			jvResult["node"]		= sleNode->getJson(0);
			jvResult["index"]		= uNodeIndex.ToString();
		}
	}

	return jvResult;
}

// {
//   ledger_hash : <ledger>
//   ledger_index : <ledger_index>
// }
Json::Value RPCHandler::doLedgerHeader(Json::Value jvRequest, int& cost)
{
	Ledger::pointer		lpLedger;
	Json::Value			jvResult	= lookupLedger(jvRequest, lpLedger);

	if (!lpLedger)
		return jvResult;

	Serializer	s;

	lpLedger->addRaw(s);

	jvResult["ledger_data"]	= strHex(s.peekData());

	// This information isn't verified, they should only use it if they trust us.
	lpLedger->addJson(jvResult, 0);

	return jvResult;
}

boost::unordered_set<RippleAddress> RPCHandler::parseAccountIds(const Json::Value& jvArray)
{
	boost::unordered_set<RippleAddress>	usnaResult;

	for (Json::Value::const_iterator it = jvArray.begin(); it != jvArray.end(); it++)
	{
		RippleAddress	naString;

		if (!(*it).isString() || !naString.setAccountID((*it).asString()))
		{
			usnaResult.clear();
			break;
		}
		else
		{
			(void) usnaResult.insert(naString);
		}
	}

	return usnaResult;
}

/*
server : Sends a message anytime the server status changes such as network connectivity.
ledger : Sends a message at every ledger close.
transactions : Sends a message for every transaction that makes it into a ledger.
rt_transactions
accounts
rt_accounts
*/
Json::Value RPCHandler::doSubscribe(Json::Value jvRequest, int& cost)
{
	InfoSub::pointer ispSub;
	Json::Value jvResult(Json::objectValue);
	uint32		uLedgerIndex = jvRequest.isMember("ledger_index") && jvRequest["ledger_index"].isNumeric()
									? jvRequest["ledger_index"].asUInt()
									: 0;

	if (!mInfoSub && !jvRequest.isMember("url"))
	{
		// Must be a JSON-RPC call.
		cLog(lsINFO) << boost::str(boost::format("doSubscribe: RPC subscribe requires a url"));

		return rpcError(rpcINVALID_PARAMS);
	}

	if (jvRequest.isMember("url"))
	{
		if (mRole != ADMIN)
			return rpcError(rpcNO_PERMISSION);

		std::string	strUrl		= jvRequest["url"].asString();
		std::string	strUsername	= jvRequest.isMember("username") ? jvRequest["username"].asString() : "";
		std::string	strPassword	= jvRequest.isMember("password") ? jvRequest["password"].asString() : "";

		ispSub	= mNetOps->findRpcSub(strUrl);
		if (!ispSub)
		{
			cLog(lsDEBUG) << boost::str(boost::format("doSubscribe: building: %s") % strUrl);

			RPCSub::pointer rspSub = boost::make_shared<RPCSub>(strUrl, strUsername, strPassword);
			ispSub	= mNetOps->addRpcSub(strUrl, boost::shared_polymorphic_downcast<InfoSub>(rspSub));
		}
		else
		{
			cLog(lsTRACE) << boost::str(boost::format("doSubscribe: reusing: %s") % strUrl);

			if (jvRequest.isMember("username"))
				dynamic_cast<RPCSub*>(&*ispSub)->setUsername(strUsername);

			if (jvRequest.isMember("password"))
				dynamic_cast<RPCSub*>(&*ispSub)->setPassword(strPassword);
		}
	}
	else
	{
		ispSub	= mInfoSub;
	}

	if (!jvRequest.isMember("streams"))
	{
		nothing();
	}
	else if (!jvRequest["streams"].isArray())
	{
		cLog(lsINFO) << boost::str(boost::format("doSubscribe: streams requires an array."));

		return rpcError(rpcINVALID_PARAMS);
	}
	else
	{
		for (Json::Value::iterator it = jvRequest["streams"].begin(); it != jvRequest["streams"].end(); it++)
		{
			if ((*it).isString())
			{
				std::string streamName=(*it).asString();

				if (streamName=="server")
				{
					mNetOps->subServer(ispSub, jvResult);
				}
				else if (streamName=="ledger")
				{
					mNetOps->subLedger(ispSub, jvResult);
				}
				else if (streamName=="transactions")
				{
					mNetOps->subTransactions(ispSub);
				}
				else if (streamName=="rt_transactions")
				{
					mNetOps->subRTTransactions(ispSub);
				}
				else
				{
					jvResult["error"]	= "unknownStream";
				}
			}
			else
			{
				jvResult["error"]	= "malformedStream";
			}
		}
	}

	if (jvRequest.isMember("rt_accounts"))
	{
		boost::unordered_set<RippleAddress> usnaAccoundIds	= parseAccountIds(jvRequest["rt_accounts"]);

		if (usnaAccoundIds.empty())
		{
			jvResult["error"]	= "malformedAccount";
		}
		else
		{
			mNetOps->subAccount(ispSub, usnaAccoundIds, uLedgerIndex, true);
		}
	}

	if (jvRequest.isMember("accounts"))
	{
		boost::unordered_set<RippleAddress> usnaAccoundIds	= parseAccountIds(jvRequest["accounts"]);

		if (usnaAccoundIds.empty())
		{
			jvResult["error"]	= "malformedAccount";
		}
		else
		{
			mNetOps->subAccount(ispSub, usnaAccoundIds, uLedgerIndex, false);

			cLog(lsDEBUG) << boost::str(boost::format("doSubscribe: accounts: %d") % usnaAccoundIds.size());
		}
	}

	if (jvRequest.isMember("books"))
	{
		for (Json::Value::iterator it = jvRequest["books"].begin(); it != jvRequest["books"].end(); it++)
		{
			uint160 currencyOut;
			STAmount::currencyFromString(currencyOut,(*it)["CurrencyOut"].asString());
			uint160 issuerOut,issuerIn;
			if(currencyOut.isNonZero())
				STAmount::issuerFromString(issuerOut,(*it)["IssuerOut"].asString());
			uint160 currencyIn;
			STAmount::currencyFromString(currencyIn,(*it)["CurrencyIn"].asString());
			if(currencyIn.isNonZero())
				STAmount::issuerFromString(issuerIn,(*it)["IssuerIn"].asString());

			bool bothSides=false;
			if((*it).isMember("BothSides") && (*it)["BothSides"].asBool()) bothSides=true;

			mNetOps->subBook(ispSub, currencyIn, currencyOut, issuerIn, issuerOut);
			if(bothSides) mNetOps->subBook(ispSub, currencyOut, currencyIn, issuerOut, issuerIn);
			if((*it)["StateNow"].asBool())
			{
				Ledger::pointer ledger= theApp->getLedgerMaster().getClosedLedger();
				RippleAddress	raTakerID;
				raTakerID.setAccountID(ACCOUNT_ONE);
				const Json::Value	jvMarker = Json::Value(Json::nullValue);
				mNetOps->getBookPage(ledger, currencyOut, issuerOut, currencyIn, issuerIn, raTakerID.getAccountID(), false, 0, jvMarker, jvResult);
				if(bothSides) 
				{
					Json::Value tempJson(Json::objectValue);
					if(jvResult.isMember("offers")) jvResult["bids"]=jvResult["offers"];
					mNetOps->getBookPage(ledger, currencyIn, issuerIn, currencyOut, issuerOut, raTakerID.getAccountID(), false, 0, jvMarker, tempJson);
					if(tempJson.isMember("offers")) jvResult["asks"]=tempJson["offers"];
				}
			}
		}
	}

	return jvResult;
}

// FIXME: This leaks RPCSub objects for JSON-RPC.  Shouldn't matter for anyone sane.
Json::Value RPCHandler::doUnsubscribe(Json::Value jvRequest, int& cost)
{
	InfoSub::pointer ispSub;
	Json::Value jvResult(Json::objectValue);

	if (!mInfoSub && !jvRequest.isMember("url"))
	{
		// Must be a JSON-RPC call.
		return rpcError(rpcINVALID_PARAMS);
	}

	if (jvRequest.isMember("url"))
	{
		if (mRole != ADMIN)
			return rpcError(rpcNO_PERMISSION);

		std::string	strUrl	= jvRequest["url"].asString();

		ispSub	= mNetOps->findRpcSub(strUrl);
		if (!ispSub)
			return jvResult;
	}
	else
	{
		ispSub	= mInfoSub;
	}

	if (jvRequest.isMember("streams"))
	{
		for (Json::Value::iterator it = jvRequest["streams"].begin(); it != jvRequest["streams"].end(); it++)
		{
			if ((*it).isString() )
			{
				std::string streamName=(*it).asString();

				if (streamName == "server")
				{
					mNetOps->unsubServer(ispSub->getSeq());
				}
				else if (streamName == "ledger")
				{
					mNetOps->unsubLedger(ispSub->getSeq());
				}
				else if (streamName == "transactions")
				{
					mNetOps->unsubTransactions(ispSub->getSeq());
				}
				else if (streamName == "rt_transactions")
				{
					mNetOps->unsubRTTransactions(ispSub->getSeq());
				}
				else
				{
					jvResult["error"]	= str(boost::format("Unknown stream: %s") % streamName);
				}
			}
			else
			{
				jvResult["error"]	= "malformedSteam";
			}
		}
	}

	if (jvRequest.isMember("rt_accounts"))
	{
		boost::unordered_set<RippleAddress> usnaAccoundIds	= parseAccountIds(jvRequest["rt_accounts"]);

		if (usnaAccoundIds.empty())
		{
			jvResult["error"]	= "malformedAccount";
		}
		else
		{
			mNetOps->unsubAccount(ispSub->getSeq(), usnaAccoundIds, true);
		}
	}

	if (jvRequest.isMember("accounts"))
	{
		boost::unordered_set<RippleAddress> usnaAccoundIds	= parseAccountIds(jvRequest["accounts"]);

		if (usnaAccoundIds.empty())
		{
			jvResult["error"]	= "malformedAccount";
		}
		else
		{
			mNetOps->unsubAccount(ispSub->getSeq(), usnaAccoundIds, false);
		}
	}

	if (jvRequest.isMember("books"))
	{
		for (Json::Value::iterator it = jvRequest["books"].begin(); it != jvRequest["books"].end(); it++)
		{
			uint160 currencyOut;
			STAmount::issuerFromString(currencyOut,(*it)["CurrencyOut"].asString());
			uint160 issuerOut = RippleAddress::createNodePublic( (*it)["IssuerOut"].asString() ).getAccountID();
			uint160 currencyIn;
			STAmount::issuerFromString(currencyOut,(*it)["CurrencyIn"].asString());
			uint160 issuerIn = RippleAddress::createNodePublic( (*it)["IssuerIn"].asString() ).getAccountID();

			mNetOps->unsubBook(ispSub->getSeq(), currencyIn, currencyOut, issuerIn, issuerOut);
		}
	}

	return jvResult;
}

// Provide the JSON-RPC "result" value.
//
// JSON-RPC provides a method and an array of params. JSON-RPC is used as a transport for a command and a request object. The
// command is the method. The request object is supplied as the first element of the params.
Json::Value RPCHandler::doRpcCommand(const std::string& strMethod, Json::Value& jvParams, int iRole, int& cost)
{
	if (cost == 0)
		cost = rpcCOST_DEFAULT;
	cLog(lsTRACE) << "doRpcCommand:" << strMethod << ":" << jvParams;

	if (!jvParams.isArray() || jvParams.size() > 1)
		return rpcError(rpcINVALID_PARAMS);

	Json::Value	jvRequest	= jvParams.size() ? jvParams[0u] : Json::Value(Json::objectValue);

	if (!jvRequest.isObject())
		return rpcError(rpcINVALID_PARAMS);

	// Provide the JSON-RPC method as the field "command" in the request.
	jvRequest["command"]	= strMethod;

	Json::Value	jvResult	= doCommand(jvRequest, iRole, cost);

	// Always report "status".  On an error report the request as received.
	if (jvResult.isMember("error"))
	{
		jvResult["status"]	= "error";
		jvResult["request"]	= jvRequest;

	} else {
		jvResult["status"]	= "success";
	}

	return jvResult;
}

Json::Value RPCHandler::doInternal(Json::Value jvRequest, int& cost)
{ // Used for debug or special-purpose RPC commands
	if (!jvRequest.isMember("internal_command"))
		return rpcError(rpcINVALID_PARAMS);
	return RPCInternalHandler::runHandler(jvRequest["internal_command"].asString(), jvRequest["params"]);
}

Json::Value RPCHandler::doCommand(const Json::Value& jvRequest, int iRole, int &cost)
{
	if (cost == 0)
		cost = rpcCOST_DEFAULT;
	if (iRole != ADMIN)
	{
		int jc = theApp->getJobQueue().getJobCountGE(jtCLIENT);
		if (jc > 500)
		{
			cLog(lsDEBUG) << "Too busy for command: " << jc;
			return rpcError(rpcTOO_BUSY);
		}
	}

	if (!jvRequest.isMember("command"))
		return rpcError(rpcCOMMAND_MISSING);

	std::string		strCommand	= jvRequest["command"].asString();

	cLog(lsTRACE) << "COMMAND:" << strCommand;
	cLog(lsTRACE) << "REQUEST:" << jvRequest;

	mRole	= iRole;

	static struct {
		const char*		pCommand;
		doFuncPtr		dfpFunc;
		bool			bAdminRequired;
		unsigned int	iOptions;
	} commandsA[] = {
		// Request-response methods
		{	"account_info",			&RPCHandler::doAccountInfo,		    false,	optCurrent	},
		{	"account_lines",		&RPCHandler::doAccountLines,	    false,	optCurrent	},
		{	"account_offers",		&RPCHandler::doAccountOffers,	    false,	optCurrent	},
		{	"account_tx",			&RPCHandler::doAccountTransactions, false,	optNetwork	},
		{	"book_offers",			&RPCHandler::doBookOffers,			false,	optCurrent	},
		{	"connect",				&RPCHandler::doConnect,			    true,	optNone		},
		{	"consensus_info",		&RPCHandler::doConsensusInfo,	    true,	optNone		},
		{	"get_counts",			&RPCHandler::doGetCounts,		    true,	optNone		},
		{	"internal",				&RPCHandler::doInternal,			true,	optNone		},
		{	"ledger",				&RPCHandler::doLedger,			    false,	optNetwork	},
		{	"ledger_accept",		&RPCHandler::doLedgerAccept,	    true,	optCurrent	},
		{	"ledger_closed",		&RPCHandler::doLedgerClosed,	    false,	optClosed	},
		{	"ledger_current",		&RPCHandler::doLedgerCurrent,	    false,	optCurrent	},
		{	"ledger_entry",			&RPCHandler::doLedgerEntry,		    false,	optCurrent	},
		{	"ledger_header",		&RPCHandler::doLedgerHeader,	    false,	optCurrent	},
		{	"log_level",			&RPCHandler::doLogLevel,		    true,	optNone		},
		{	"logrotate",			&RPCHandler::doLogRotate,		    true,	optNone		},
//		{	"nickname_info",		&RPCHandler::doNicknameInfo,	    false,	optCurrent	},
		{	"owner_info",			&RPCHandler::doOwnerInfo,		    false,	optCurrent	},
		{	"peers",				&RPCHandler::doPeers,			    true,	optNone		},
		{	"ping",					&RPCHandler::doPing,			    false,	optNone		},
//		{	"profile",				&RPCHandler::doProfile,			    false,	optCurrent	},
		{	"random",				&RPCHandler::doRandom,				false,	optNone		},
		{	"ripple_path_find",		&RPCHandler::doRipplePathFind,	    false,	optCurrent	},
		{	"sign",					&RPCHandler::doSign,			    false,	optCurrent	},
		{	"submit",				&RPCHandler::doSubmit,			    false,	optCurrent	},
		{	"server_info",			&RPCHandler::doServerInfo,		    false,	optNone		},
		{	"server_state",			&RPCHandler::doServerState,		    false,	optNone		},
		{	"stop",					&RPCHandler::doStop,			    true,	optNone		},
		{	"transaction_entry",	&RPCHandler::doTransactionEntry,    false,	optCurrent	},
		{	"tx",					&RPCHandler::doTx,				    false,	optNetwork	},
		{	"tx_history",			&RPCHandler::doTxHistory,		    false,	optNone		},

		{	"unl_add",				&RPCHandler::doUnlAdd,			    true,	optNone		},
		{	"unl_delete",			&RPCHandler::doUnlDelete,		    true,	optNone		},
		{	"unl_list",				&RPCHandler::doUnlList,			    true,	optNone		},
		{	"unl_load",				&RPCHandler::doUnlLoad,			    true,	optNone		},
		{	"unl_network",			&RPCHandler::doUnlNetwork,		    true,	optNone		},
		{	"unl_reset",			&RPCHandler::doUnlReset,		    true,	optNone		},
		{	"unl_score",			&RPCHandler::doUnlScore,		    true,	optNone		},

		{	"validation_create",	&RPCHandler::doValidationCreate,    true,	optNone		},
		{	"validation_seed",		&RPCHandler::doValidationSeed,	    true,	optNone		},

		{	"wallet_accounts",		&RPCHandler::doWalletAccounts,	    false,	optCurrent	},
		{	"wallet_propose",		&RPCHandler::doWalletPropose,	    false,	optNone		},
		{	"wallet_seed",			&RPCHandler::doWalletSeed,		    false,	optNone		},

#if ENABLE_INSECURE
		// XXX Unnecessary commands which should be removed.
		{	"login",				&RPCHandler::doLogin,			    true,	optNone		},
		{	"data_delete",			&RPCHandler::doDataDelete,		    true,	optNone		},
		{	"data_fetch",			&RPCHandler::doDataFetch,		    true,	optNone		},
		{	"data_store",			&RPCHandler::doDataStore,		    true,	optNone		},
#endif

		// Evented methods
		{	"subscribe",			&RPCHandler::doSubscribe,			false,	optNone		},
		{	"unsubscribe",			&RPCHandler::doUnsubscribe,			false,	optNone		},
	};

	int		i = NUMBER(commandsA);

	while (i-- && strCommand != commandsA[i].pCommand)
		;

	if (i < 0)
	{
		return rpcError(rpcUNKNOWN_COMMAND);
	}
	else if (commandsA[i].bAdminRequired && mRole != ADMIN)
	{
		return rpcError(rpcNO_PERMISSION);
	}

	boost::recursive_mutex::scoped_lock sl(theApp->getMasterLock());

	if (commandsA[i].iOptions & optNetwork
		&& mNetOps->getOperatingMode() != NetworkOPs::omTRACKING
		&& mNetOps->getOperatingMode() != NetworkOPs::omFULL)
	{
		cLog(lsINFO) << "Insufficient network mode for RPC: " << mNetOps->strOperatingMode();

		return rpcError(rpcNO_NETWORK);
	}
	// XXX Should verify we have a current ledger.

	if ((commandsA[i].iOptions & optCurrent) && false)
	{
		return rpcError(rpcNO_CURRENT);
	}
	else if ((commandsA[i].iOptions & optClosed) && !mNetOps->getClosedLedger())
	{
		return rpcError(rpcNO_CLOSED);
	}
	else
	{
		try {
			Json::Value	jvRaw		= (this->*(commandsA[i].dfpFunc))(jvRequest, cost);

			// Regularize result.
			if (jvRaw.isObject())
			{
				// Got an object.
				return jvRaw;
			}
			else
			{
				// Probably got a string.
				Json::Value	jvResult(Json::objectValue);

				jvResult["message"]	= jvRaw;

				return jvResult;
			}
		}
		catch (std::exception& e)
		{
			cLog(lsINFO) << "Caught throw: " << e.what();
			if (cost == rpcCOST_DEFAULT)
				cost = rpcCOST_EXCEPTION;

			return rpcError(rpcINTERNAL);
		}
	}
}

RPCInternalHandler* RPCInternalHandler::sHeadHandler = NULL;

RPCInternalHandler::RPCInternalHandler(const std::string& name, handler_t Handler) : mName(name), mHandler(Handler)
{
	mNextHandler = sHeadHandler;
	sHeadHandler = this;
}

Json::Value RPCInternalHandler::runHandler(const std::string& name, const Json::Value& params)
{
	RPCInternalHandler* h = sHeadHandler;
	while (h != NULL)
	{
		if (name == h->mName)
		{
			cLog(lsWARNING) << "Internal command " << name << ": " << params;
			Json::Value ret = h->mHandler(params);
			cLog(lsWARNING) << "Internal command returns: " << ret;
			return ret;
		}
		h = h->mNextHandler;
	}
	return rpcError(rpcBAD_SYNTAX);
}

// vim:ts=4
