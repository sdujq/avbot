/*
 * Copyright (C) 2012 - 2013  微蔡 <microcai@fedoraproject.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include <string.h>
#include <string>
#include <iostream>

#include <boost/log/trivial.hpp>
#include <boost/random.hpp>
#include <boost/system/system_error.hpp>
#include <boost/bind.hpp>
#include <boost/bind/protect.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/format.hpp>
#include <boost/property_tree/json_parser.hpp>
namespace js = boost::property_tree::json_parser;
#include <boost/algorithm/string.hpp>
#include <boost/foreach.hpp>
#include <boost/assign.hpp>
#include <boost/scope_exit.hpp>
#include <boost/regex/pending/unicode_iterator.hpp>

#include "boost/timedcall.hpp"
#include "boost/multihandler.hpp"
#include "boost/consolestr.hpp"
#include "boost/urlencode.hpp"

#include "constant.hpp"
#include "../webqq.h"

#include "webqq_impl.hpp"
#include "md5.hpp"
#include "utf8.hpp"
#include "lwqq_status.hpp"
#include "webqq_login.hpp"
#include "clean_cache.hpp"

#include "process_group_msg.hpp"

#ifdef WIN32

#include <stdarg.h>

// '_vsnprintf': This function or variable may be unsafe
#pragma warning(disable:4996)

inline int snprintf(char* buf, int len, char const* fmt, ...)
{
	va_list lp;
	va_start(lp, fmt);
	int ret = _vsnprintf(buf, len, fmt, lp);
	va_end(lp);
	if (ret < 0) { buf[len-1] = 0; ret = len-1; }
	return ret;
}

#endif // WIN32

static void dummy(){}

using namespace qqimpl;

static std::string generate_clientid();

///low level special char mapping
static std::string parse_unescape(const std::string &);

static std::string create_post_data( std::string vfwebqq )
{
	std::string m = boost::str( boost::format( "{\"vfwebqq\":\"%s\"}" ) % vfwebqq );
	return std::string( "r=" ) + boost::url_encode(m);
}

static pt::wptree json_parse( const wchar_t * doc )
{
	pt::wptree jstree;
	std::wstringstream stream;
	stream <<  doc ;
	js::read_json( stream, jstree );
	return jstree;
}

// build webqq and setup defaults
WebQQ::WebQQ( boost::asio::io_service& _io_service,
			  std::string _qqnum, std::string _passwd)
	: m_io_service( _io_service ), m_qqnum( _qqnum ), m_passwd( _passwd ), m_status( LWQQ_STATUS_OFFLINE ),
	m_msg_queue( 20 ) //　最多保留最后的20条未发送消息.
{
#ifndef _WIN32
	/* Set msg_id */
	timeval tv;
	long v;
	gettimeofday( &tv, NULL );
	v = tv.tv_usec;
	v = ( v - v % 1000 ) / 1000;
	v = v % 10000 * 10000;
	m_msg_id = v;
#else
	m_msg_id = std::rand();
#endif

	init_face_map();

	if (!boost::filesystem::exists("cache"))
		boost::filesystem::create_directories("cache");

	// 开启个程序去清理过期 cache_* 文件
	// webqq 每天登录 uid 变化,  而不是每次都变化.
	// 所以 cache 有效期只有一天.
	clean_cache(get_ioservice());
}

/**login*/
void WebQQ::login()
{
	m_cookies.clear();
	// start login process, will call login_withvc later
	if (m_status == LWQQ_STATUS_OFFLINE)
		detail::corologin op( shared_from_this() );
}

// login to server with vc. called by login code or by user
// if no verify image needed, then called by login
// if verify image needed, then the user should listen to signeedvc and call this
void WebQQ::login_withvc( std::string vccode )
{
	std::cout << "vc code is \"" << vccode << "\"" << std::endl;
	detail::corologin_vc op( shared_from_this(), vccode );
}

// last step of a login process
// and this will be callded every other minutes to prevent foce kick off.
void  WebQQ::change_status(LWQQ_STATUS status, boost::function<void (boost::system::error_code) > handler)
{
	detail::lwqq_change_status op(shared_from_this(), status, handler);
}

void WebQQ::send_group_message( qqGroup& group, std::string msg, send_group_message_cb donecb )
{
	send_group_message( group.gid, msg, donecb );
}

void WebQQ::send_group_message( std::string group, std::string msg, send_group_message_cb donecb )
{
	//check if already in sending a message
	m_msg_queue.push_back( boost::make_tuple( group, msg, donecb ) );

	if( !m_group_msg_insending ) {
		m_group_msg_insending = true;
		send_group_message_internal( group, msg, donecb );
	}
}

void WebQQ::send_group_message_internal( std::string group, std::string msg, send_group_message_cb donecb )
{
	//unescape for POST
	std::string messagejson = boost::str(
					boost::format("{\"group_uin\":\"%s\", "
									"\"content\":\"["
									"\\\"%s\\\","
									"[\\\"font\\\",{\\\"name\\\":\\\"宋体\\\",\\\"size\\\":\\\"9\\\",\\\"style\\\":[0,0,0],\\\"color\\\":\\\"000000\\\"}]"
									"]\","
									"\"msg_id\":%ld,"
									"\"clientid\":\"%s\","
									"\"psessionid\":\"%s\"}")
							%group % parse_unescape( msg ) % m_msg_id % m_clientid % m_psessionid
						);

	std::string postdata =  boost::str(
							   boost::format( "r=%s&clientid=%s&psessionid=%s" )
							   % boost::url_encode(messagejson)
							   % m_clientid
							   % m_psessionid
						   );

	read_streamptr stream( new avhttp::http_stream( m_io_service ) );
	stream->request_options(
		avhttp::request_opts()
		( avhttp::http_options::request_method, "POST" )
		( avhttp::http_options::cookie, m_cookies.lwcookies )
		( avhttp::http_options::referer, "http://d.web2.qq.com/proxy.html?v=20101025002" )
		( avhttp::http_options::content_type, "application/x-www-form-urlencoded; charset=UTF-8" )
		( avhttp::http_options::request_body, postdata )
		( avhttp::http_options::content_length, boost::lexical_cast<std::string>( postdata.length() ) )
		( avhttp::http_options::connection, "close" )
	);

	boost::shared_ptr<boost::asio::streambuf> buffer = boost::make_shared<boost::asio::streambuf>();

	avhttp::async_read_body( *stream, LWQQ_URL_SEND_QUN_MSG, *buffer,
						 boost::bind( &WebQQ::cb_send_msg, this, _1, stream, buffer, donecb )
					   );
}

void WebQQ::cb_send_msg( const boost::system::error_code& ec, read_streamptr stream, boost::shared_ptr<boost::asio::streambuf> buffer, boost::function<void ( const boost::system::error_code& ec )> donecb )
{
	pt::ptree jstree;
	std::istream	response( buffer.get() );

	try {
		js::read_json( response, jstree );

		if( jstree.get<int>( "retcode" ) == 108 ) {
			// 已经断线，重新登录
			m_status = LWQQ_STATUS_OFFLINE;
			m_cookies.clear();
			// 10s 后登录.
			boost::delayedcallsec( m_io_service, 10, boost::bind( &WebQQ::login, this ) );
			m_group_msg_insending = false;
			return ;
		}

	} catch( const pt::json_parser_error & jserr ) {
		std::istream	response( buffer.get() );
		BOOST_LOG_TRIVIAL(error) <<  __FILE__ << " : " << __LINE__ << " : " << "parse json error : " << jserr.what()
 			<<  "\n=========\n" <<  jserr.message() << "\n=========" ;
		m_msg_queue.pop_front();
	} catch( const pt::ptree_bad_path & badpath ) {
		BOOST_LOG_TRIVIAL(error) << __FILE__ << " : " << __LINE__ << " : " <<  "bad path " <<  badpath.what();
	}

	if (!m_msg_queue.empty())
		m_msg_queue.pop_front();

	if( m_msg_queue.empty() ) {
		m_group_msg_insending = false;
	} else {
		boost::tuple<std::string, std::string, send_group_message_cb> v = m_msg_queue.front();
		boost::delayedcallms( m_io_service, 500, boost::bind( &WebQQ::send_group_message_internal, this, boost::get<0>( v ), boost::get<1>( v ), boost::get<2>( v ) ) );
	}

	m_io_service.post( boost::asio::detail::bind_handler( donecb, ec ) );
}

void WebQQ::update_group_list()
{
	std::cout <<  "getting group list" <<  std::endl;
	/* Create post data: {"h":"hello","vfwebqq":"4354j53h45j34"} */
	std::string postdata = create_post_data( this->m_vfwebqq );
	std::string url = boost::str( boost::format( "%s/api/get_group_name_list_mask2" ) % "http://s.web2.qq.com" );

	read_streamptr stream( new avhttp::http_stream( m_io_service ) );
	stream->request_options(
		avhttp::request_opts()
		( avhttp::http_options::request_method, "POST" )
		( avhttp::http_options::cookie, m_cookies.lwcookies )
		( avhttp::http_options::referer, "http://s.web2.qq.com/proxy.html?v=20110412001&callback=1&id=1" )
		( avhttp::http_options::content_type, "application/x-www-form-urlencoded; charset=UTF-8" )
		( avhttp::http_options::request_body, postdata )
		( avhttp::http_options::content_length, boost::lexical_cast<std::string>( postdata.length() ) )
		( avhttp::http_options::connection, "close" )
	);

	boost::shared_ptr<boost::asio::streambuf> buffer = boost::make_shared<boost::asio::streambuf>();

	avhttp::async_read_body( *stream, url, *buffer,
						 boost::bind( &WebQQ::cb_group_list, this, _1, stream, buffer )
					   );
}

void WebQQ::update_group_qqnumber(boost::shared_ptr<qqGroup> group )
{
	std::string url;

	url = boost::str(
			  boost::format( "%s/api/get_friend_uin2?tuin=%s&verifysession=&type=1&code=&vfwebqq=%s&t=%ld" )
			  % "http://s.web2.qq.com"
			  % group->code
			  % m_vfwebqq
			  % time( NULL )
		  );
	read_streamptr stream( new avhttp::http_stream( m_io_service ) );
	stream->request_options(
		avhttp::request_opts()
		( avhttp::http_options::cookie, m_cookies.lwcookies )
		( avhttp::http_options::referer, LWQQ_URL_REFERER_QUN_DETAIL )
		( avhttp::http_options::connection, "close" )
	);
	boost::shared_ptr<boost::asio::streambuf> buffer = boost::make_shared<boost::asio::streambuf>();

	avhttp::async_read_body( *stream, url, *buffer,
						 boost::bind( &WebQQ::cb_group_qqnumber, this, _1, stream, buffer, group)
					   );
}

void WebQQ::update_group_member(boost::shared_ptr<qqGroup> group , done_callback_handler handler)
{
	read_streamptr stream( new avhttp::http_stream( m_io_service ) );

	std::string url = boost::str(
						  boost::format( "%s/api/get_group_info_ext2?gcode=%s&vfwebqq=%s&t=%ld" )
						  % "http://s.web2.qq.com"
						  % group->code
						  % m_vfwebqq
						  % std::time( NULL )
					  );
	stream->request_options(
		avhttp::request_opts()
		( avhttp::http_options::cookie, m_cookies.lwcookies )
		( avhttp::http_options::referer, LWQQ_URL_REFERER_QUN_DETAIL )
		( avhttp::http_options::connection, "close" )
	);

	boost::shared_ptr<boost::asio::streambuf> buffer = boost::make_shared<boost::asio::streambuf>();

	avhttp::async_read_body( *stream, url, * buffer,
						 boost::bind( &WebQQ::cb_group_member, this, _1, stream, buffer, group, handler)
					   );
}

class SYMBOL_HIDDEN buddy_uin_to_qqnumber {
public:
	typedef void result_type;
	// 将　qqBuddy 里的　uin 转化为　qq 号码.
	template<class Handler>
	buddy_uin_to_qqnumber( boost::shared_ptr<WebQQ> _webqq, std::string uin, Handler handler )
		: _io_service( _webqq->get_ioservice() ) {
		read_streamptr stream;
		std::string url = boost::str(
							  boost::format( "%s/api/get_friend_uin2?tuin=%s&verifysession=&type=1&code=&vfwebqq=%s" )
							  % "http://s.web2.qq.com" % uin % _webqq->m_vfwebqq
						  );

		stream.reset( new avhttp::http_stream( _webqq->get_ioservice() ) );
		stream->request_options(
			avhttp::request_opts()
			( avhttp::http_options::http_version , "HTTP/1.0" )
			( avhttp::http_options::cookie, _webqq->m_cookies.lwcookies )
			( avhttp::http_options::referer, "http://s.web2.qq.com/proxy.html?v=201304220930&callback=1&id=3" )
			( avhttp::http_options::content_type, "UTF-8" )
			( avhttp::http_options::connection, "close" )
		);

		boost::shared_ptr<boost::asio::streambuf> buffer = boost::make_shared<boost::asio::streambuf>();

		avhttp::async_read_body(*stream, url, *buffer, boost::bind( *this, _1, stream, buffer, handler ) );
	}

	template <class Handler>
	void operator()( const boost::system::error_code& ec, read_streamptr stream, boost::shared_ptr<boost::asio::streambuf> buffer, Handler handler )
	{
		// 获得的返回代码类似
		// {"retcode":0,"result":{"uiuin":"","account":2664046919,"uin":721281587}}
		pt::ptree jsonobj;
		std::iostream resultjson( buffer.get() );

		try {
			// 处理.
			pt::json_parser::read_json( resultjson, jsonobj );
			int retcode = jsonobj.get<int>("retcode");
			if (retcode ==  99999 || retcode ==  100000 ){
				_io_service.post( boost::asio::detail::bind_handler( handler, std::string("-1") ) );
			}else{
				std::string qqnum = jsonobj.get<std::string>( "result.account" );

				_io_service.post( boost::asio::detail::bind_handler( handler, qqnum ) );
			}
			return ;
		} catch( const pt::json_parser_error & jserr ) {
			BOOST_LOG_TRIVIAL(error) <<  __FILE__ << " : " << __LINE__ << " : " << "parse json error : " <<  jserr.what();
		} catch( const pt::ptree_bad_path & badpath ) {
			BOOST_LOG_TRIVIAL(error) <<  __FILE__ << " : " << __LINE__ << " : " <<  "bad path" <<  badpath.what();
			js::write_json( std::cout, jsonobj );
		}

		_io_service.post( boost::asio::detail::bind_handler( handler, std::string( "" ) ) );
	}
private:
	boost::asio::io_service& _io_service;
};

class SYMBOL_HIDDEN update_group_member_qq : boost::asio::coroutine {
public:
	typedef void result_type;

	update_group_member_qq( boost::shared_ptr<WebQQ>  _webqq, boost::shared_ptr<qqGroup> _group )
		: group( _group ), m_webqq( _webqq ) {
		m_webqq->get_ioservice().post( boost::bind( *this, "" ) );
	}

	void operator()( std::string qqnum )
	{
		//我说了是一个一个的更新对吧，可不能一次发起　N 个连接同时更新，会被TX拉黑名单的.
		reenter( this )
		{
			for( it = group->memberlist.begin(); it != group->memberlist.end(); it++ ) {
				if (it->second.qqnum.empty())
				{
					yield buddy_uin_to_qqnumber( m_webqq, it->second.uin, *this );
					if ( qqnum == "-1")
					return;
				}
				it->second.qqnum = qqnum;
			}
		}
	}
private:
	std::map< std::string, qqBuddy >::iterator it;
	boost::shared_ptr<qqGroup> group;
	boost::shared_ptr<WebQQ> m_webqq;
};

//　将组成员的 QQ 号码一个一个更新过来.
void WebQQ::update_group_member_qq(boost::shared_ptr<qqGroup> group )
{
	::update_group_member_qq( shared_from_this(), group );
}

qqGroup_ptr WebQQ::get_Group_by_gid( std::string gid )
{
	grouplist::iterator it = m_groups.find( gid );

	if( it != m_groups.end() )
		return it->second;

	return qqGroup_ptr();
}

qqGroup_ptr WebQQ::get_Group_by_qq( std::string qq )
{
	grouplist::iterator it = m_groups.begin();

	for( ; it != m_groups.end(); it ++ ) {
		if( it->second->qqnum == qq )
			return it->second;
	}

	return qqGroup_ptr();
}


void WebQQ::get_verify_image( std::string vcimgid )
{
	if( vcimgid.length() < 8 ) {
		m_status = LWQQ_STATUS_OFFLINE;
		boost::delayedcallsec( m_io_service, 10, boost::bind( &WebQQ::login, this ) );
		return ;
	}

	std::string url = boost::str(
						  boost::format( LWQQ_URL_VERIFY_IMG ) % APPID % m_qqnum
					  );

	read_streamptr stream( new avhttp::http_stream( m_io_service ) );
	stream->request_options(
		avhttp::request_opts()
		( avhttp::http_options::cookie, std::string( "chkuin=" ) + m_qqnum )
		( avhttp::http_options::connection, "close" )
	);
	boost::shared_ptr<boost::asio::streambuf> buffer = boost::make_shared<boost::asio::streambuf>();
	avhttp::async_read_body( *stream, url, * buffer,
						 boost::bind( &WebQQ::cb_get_verify_image, this, _1, stream, buffer ) );
}

void WebQQ::cb_get_verify_image( const boost::system::error_code& ec, read_streamptr stream, boost::shared_ptr<boost::asio::streambuf> buffer )
{
	detail::update_cookies( &m_cookies, stream->response_options().header_string() , "verifysession");
	m_cookies.update();

	// verify image is now in response
	signeedvc( buffer->data() );
}

void WebQQ::do_poll_one_msg( std::string ptwebqq )
{
	/* Create a POST request */
	std::string msg = boost::str(
						  boost::format( "{\"clientid\":\"%s\",\"psessionid\":\"%s\"}" )
						  % m_clientid
						  % m_psessionid
					  );

	msg = boost::str( boost::format( "r=%s\r\n" ) %  boost::url_encode(msg) );

	read_streamptr pollstream( new avhttp::http_stream( m_io_service ) );
	pollstream->request_options( avhttp::request_opts()
								 ( avhttp::http_options::request_method, "POST" )
								 ( avhttp::http_options::cookie, m_cookies.lwcookies )
								 ( "cookie2", "$Version=1" )
								 ( avhttp::http_options::referer, "http://d.web2.qq.com/proxy.html?v=20101025002" )
								 ( avhttp::http_options::request_body, msg )
								 ( avhttp::http_options::content_type, "application/x-www-form-urlencoded; charset=UTF-8" )
								 ( avhttp::http_options::content_length, boost::lexical_cast<std::string>( msg.length() ) )
								 ( avhttp::http_options::connection, "close" )
							   );
	boost::shared_ptr<boost::asio::streambuf> buffer = boost::make_shared<boost::asio::streambuf>();

	avhttp::async_read_body( *pollstream, "http://d.web2.qq.com/channel/poll2", * buffer,
							boost::bind( &WebQQ::cb_poll_msg, this, _1, pollstream, buffer, ptwebqq )
					   );
}

void WebQQ::cb_poll_msg( const boost::system::error_code& ec, read_streamptr stream, boost::shared_ptr<boost::asio::streambuf> buf, std::string ptwebqq)
{
	if( ptwebqq != m_cookies.ptwebqq ) {
		BOOST_LOG_TRIVIAL(info) << "stoped polling messages" <<  std::endl;
		return ;
	}

	if ( ec ){
		// 出现网络错误, 重试.
		//开启新的 poll
		do_poll_one_msg(ptwebqq);
		return;
	}

	std::wstring response = utf8_wide( std::string( boost::asio::buffer_cast<const char*>( buf->data() ) , buf->size() ) );

	pt::wptree	jsonobj;

	std::wstringstream jsondata;
	jsondata << response;

	//处理!
	try
	{
		pt::json_parser::read_json( jsondata, jsonobj );
		process_msg( jsonobj, ptwebqq );
		//开启新的 poll
		do_poll_one_msg(ptwebqq);

	}
	catch( const pt::file_parser_error & jserr )
	{
		BOOST_LOG_TRIVIAL(error) <<  __FILE__ << " : " << __LINE__ << " : " << "parse json error : " <<  jserr.what();
		// 网络可能出了点问题，延时重试.
		boost::delayedcallsec( get_ioservice(), 5, boost::bind( &WebQQ::do_poll_one_msg, this, ptwebqq ) );
	}
	catch( const pt::ptree_error & badpath )
	{
		BOOST_LOG_TRIVIAL(error) <<  __FILE__ << " : " << __LINE__ << " : " <<  "bad path" <<  badpath.what();
		js::write_json( std::wcout, jsonobj );
		//开启新的 poll
		boost::delayedcallsec( get_ioservice(), 1, boost::bind( &WebQQ::do_poll_one_msg, this, ptwebqq ) );

	}
}

void WebQQ::process_group_message( const boost::property_tree::wptree& jstree )
{
	qqimpl::detail::process_group_message_op(*this, jstree);
}

static void cb_get_msg_tip(const boost::system::error_code& ec, std::size_t bytes_transfered, read_streamptr stream, boost::shared_ptr<boost::asio::streambuf> buffer)
{
	// 忽略错误.
}

void WebQQ::process_msg( const pt::wptree &jstree , std::string & ptwebqq )
{
	//在这里解析json数据.
	int retcode = jstree.get<int>( L"retcode" );

	if( retcode ) {
		if( retcode == 116)
		{
			// 更新 ptwebqq
			ptwebqq = this->m_cookies.ptwebqq = wide_utf8( jstree.get<std::wstring>( L"p") );
			m_cookies.update();
			
		}else if( retcode == 102 )
		{
			// 搞一个 GET 的长维护
			read_streamptr get_msg_tip( new avhttp::http_stream( m_io_service ) );
			get_msg_tip->request_options( avhttp::request_opts()
										  ( avhttp::http_options::cookie, m_cookies.lwcookies )
										  ( avhttp::http_options::referer, "http://d.web2.qq.com/proxy.html?v=20101025002" )
										  ( avhttp::http_options::connection, "close" )
										);
			boost::shared_ptr<boost::asio::streambuf> buffer = boost::make_shared<boost::asio::streambuf>();

			avhttp::async_read_body( *get_msg_tip, "http://webqq.qq.com/web2/get_msg_tip?uin=&tp=1&id=0&retype=1&rc=1&lv=3&t=1348458711542",
								* buffer, boost::bind(&cb_get_msg_tip, _1, _2, get_msg_tip, buffer));
		}
		else
		{
			m_status = LWQQ_STATUS_OFFLINE;
			m_cookies.clear();
			boost::delayedcallsec( m_io_service, 15, boost::bind( &WebQQ::login, this ) );
			js::write_json(std::wcerr, jstree);
		}

		return;
	}

	BOOST_FOREACH( const pt::wptree::value_type & result, jstree.get_child( L"result" ) ) {
		std::string poll_type = wide_utf8( result.second.get<std::wstring>( L"poll_type" ) );

 		if (poll_type != "group_message"){
			js::write_json( std::wcout, jstree );
		}
		if( poll_type == "group_message" ) {
			process_group_message( result.second );
		} else if( poll_type == "sys_g_msg" ) {
			//群消息.
			if( result.second.get<std::wstring>( L"value.type" ) == L"group_join" )
			{
				// 新人进来 !
				// 检查一下新人.
				// 这个是群号.
				std::wstring groupnumber = result.second.get<std::wstring>(L"value.t_gcode");
				std::wstring newuseruid = result.second.get<std::wstring>(L"value.new_member");
				qqGroup_ptr group = get_Group_by_qq(wide_utf8(groupnumber));

				// 报告一下新人入群!
				update_group_member(group, boost::bind(&WebQQ::cb_newbee_group_join, shared_from_this(), group, wide_utf8(newuseruid)));
			}else if(result.second.get<std::wstring>( L"value.type" ) == L"group_leave")
			{
				// 旧人滚蛋.

			}
		} else if( poll_type == "buddylist_change" ) {
			//群列表变化了，reload列表.
			js::write_json( std::wcout, result.second );
		} else if( poll_type == "kick_message" ) {
			js::write_json( std::wcout, result.second );
			//强制下线了，重登录.
			if (m_status == LWQQ_STATUS_ONLINE){
				m_status = LWQQ_STATUS_OFFLINE;
				m_cookies.ptwebqq = "";
				boost::delayedcallsec( m_io_service, 15, boost::bind( &WebQQ::login, shared_from_this() ) );
			}
		}
	}
}

void WebQQ::cb_group_list( const boost::system::error_code& ec, read_streamptr stream, boost::shared_ptr<boost::asio::streambuf> buffer )
{
	pt::ptree	jsonobj;
	std::istream jsondata( buffer.get() );
	bool retry = false;

	if (!ec){
		//处理!
		try {
			pt::json_parser::read_json( jsondata, jsonobj );

			//TODO, group list
			if( !( retry = !( jsonobj.get<int>( "retcode" ) == 0 ) ) ) {
				BOOST_FOREACH( pt::ptree::value_type result,
							jsonobj.get_child( "result" ).get_child( "gnamelist" ) ) {
					boost::shared_ptr<qqGroup>	newgroup(new qqGroup);
					newgroup->gid = result.second.get<std::string>( "gid" );
					newgroup->name = result.second.get<std::string>( "name" );
					newgroup->code = result.second.get<std::string>( "code" );

					if( newgroup->gid[0] == '-' ) {
						retry = true;
						BOOST_LOG_TRIVIAL(error) <<  __FILE__ << " : " << __LINE__ << " : " <<  "qqGroup get error" << std::endl;
						continue;
					}

					this->m_groups.insert( std::make_pair( newgroup->gid, newgroup ) );
					BOOST_LOG_TRIVIAL(info) <<  __FILE__ << " : " << __LINE__ << " : " << console_out_str("qq群 ") << console_out_str(newgroup->gid) <<  console_out_str(newgroup->name);

				}
			}
		} catch( const pt::json_parser_error & jserr ) {
			retry = true;
			BOOST_LOG_TRIVIAL(error) << __FILE__ << " : " << __LINE__ << " : " <<  "parse json error : " <<  console_out_str(jserr.what());
		} catch( const pt::ptree_bad_path & badpath ) {
			retry = true;
			BOOST_LOG_TRIVIAL(error) << __FILE__ << " : " << __LINE__ << " : " <<   "bad path error " <<  badpath.what();
		}
	}else
		retry = 1;

	if( retry ) {
		boost::delayedcallsec( m_io_service, 5, boost::bind( &WebQQ::update_group_list, shared_from_this() ) );
	} else {
		int groupcount = m_groups.size();

		done_callback_handler groupmembercb = boost::bindmultihandler(groupcount, boost::bind( &WebQQ::do_poll_one_msg, shared_from_this(), m_cookies.ptwebqq ));
		// fetching more budy info.
		BOOST_FOREACH( grouplist::value_type & v, m_groups ) {
			update_group_qqnumber( v.second );
			update_group_member( v.second , groupmembercb);
		}
	}
}

void WebQQ::cb_group_qqnumber( const boost::system::error_code& ec, read_streamptr stream, boost::shared_ptr<boost::asio::streambuf> buffer, boost::shared_ptr<qqGroup> group)
{
	pt::ptree	jsonobj;
	std::istream jsondata( buffer.get() );

	/**
	 * Here, we got a json object like this:
	 * {"retcode":0,"result":{"uiuin":"","account":615050000,"uin":954663841}}
	 *
	 */
	//处理!
	try {
		pt::json_parser::read_json( jsondata, jsonobj );

		//TODO, group members
		if( jsonobj.get<int>( "retcode" ) == 0 ) {
			group->qqnum = jsonobj.get<std::string>( "result.account" );
			BOOST_LOG_TRIVIAL(debug) <<  "qq number of group " <<  console_out_str(group->name) << " is " <<  group->qqnum;
			// 写缓存
			pt::json_parser::write_json(std::string("cache/group_qqnumber") + group->gid, jsonobj);
			//start polling messages, 2 connections!
			BOOST_LOG_TRIVIAL(info) << "start polling messages";

			boost::delayedcallsec( get_ioservice(), 3, boost::bind( &WebQQ::do_poll_one_msg, shared_from_this(), m_cookies.ptwebqq ) );

			siggroupnumber(group);

			return ;
		}else{
			BOOST_LOG_TRIVIAL(error) << console_out_str("获取群的QQ号码失败");
			pt::json_parser::write_json(std::cerr, jsonobj);
		}
	} catch( const pt::json_parser_error & jserr ) {

	} catch( const pt::ptree_bad_path & badpath ) {
	}

	try{
	// 读取缓存
		pt::json_parser::read_json(std::string("cache/group_qqnumber") + group->gid, jsonobj);

		group->qqnum = jsonobj.get<std::string>( "result.account" );
		BOOST_LOG_TRIVIAL(debug) <<  "(cached) qq number of group" <<  console_out_str(group->name) << "is" <<  group->qqnum << std::endl;

		// 向用户报告一个 group 出来了.
		siggroupnumber(group);
		return;
	}catch (...){
		boost::delayedcallsec( m_io_service, 50 + boost::rand48()() % 100 , boost::bind( &WebQQ::update_group_qqnumber, shared_from_this(), group) );
	}
}

void WebQQ::cb_newbee_group_join( qqGroup_ptr group,  std::string uid )
{
	// 报告新人入群.
	signewbuddy(group, group->get_Buddy_by_uin(uid));
}

void WebQQ::cb_group_member_process_json(pt::ptree &jsonobj, boost::shared_ptr<qqGroup> group)
{
	//TODO, group members
	if( jsonobj.get<int>( "retcode" ) == 0 ) {
		group->owner = jsonobj.get<std::string>( "result.ginfo.owner" );

		BOOST_FOREACH( pt::ptree::value_type & v, jsonobj.get_child( "result.minfo" ) ) {
			qqBuddy buddy;
			pt::ptree & minfo = v.second;
			buddy.nick = minfo.get<std::string>( "nick" );
			buddy.uin = minfo.get<std::string>( "uin" );

			group->memberlist.insert( std::make_pair( buddy.uin, buddy ) );
		}

		BOOST_FOREACH( pt::ptree::value_type & v, jsonobj.get_child( "result.ginfo.members" ) ) {
			pt::ptree & minfo = v.second;
			std::string muin = minfo.get<std::string>( "muin" );
			std::string mflag = minfo.get<std::string>( "mflag" );

			try {
				group->get_Buddy_by_uin( muin )->mflag = boost::lexical_cast<unsigned int>( mflag );
			} catch( boost::bad_lexical_cast & e ) {}
		}
		try {
			BOOST_FOREACH( pt::ptree::value_type & v, jsonobj.get_child( "result.cards" ) )
			{
				pt::ptree & minfo = v.second;
				std::string muin = minfo.get<std::string>( "muin" );
				std::string card = minfo.get<std::string>( "card" );
				group->get_Buddy_by_uin( muin )->card = card;
			}
		} catch( const pt::ptree_bad_path & badpath ) {
		}
	}
}


void WebQQ::cb_group_member( const boost::system::error_code& ec, read_streamptr stream, boost::shared_ptr<boost::asio::streambuf> buffer, boost::shared_ptr<qqGroup> group, done_callback_handler handler)
{
	//处理!
	try {

		pt::ptree jsonobj;
		std::istream jsondata( buffer.get() );

		pt::json_parser::read_json( jsondata, jsonobj );

		cb_group_member_process_json(jsonobj, group);

		pt::json_parser::write_json( std::string("cache/group_") + console_out_str(group->name) , jsonobj );

		// 开始更新成员的 QQ 号码，一次更新一个，慢慢来.
		this->update_group_member_qq( group );

		get_ioservice().post(handler);

	} catch( const pt::json_parser_error & jserr ) {
		BOOST_LOG_TRIVIAL(error) <<  __FILE__ << " : " << __LINE__ << " : " <<  "parse json error : " <<  jserr.what();

		boost::delayedcallsec( m_io_service, 20, boost::bind( &WebQQ::update_group_member, shared_from_this(), group, dummy) );
		// 在重试之前，获取缓存文件.
		try{
		pt::ptree jsonobj;
		pt::json_parser::read_json(std::string("cache/group_") + group->name , jsonobj);
		cb_group_member_process_json(jsonobj, group);
		}catch (...){}
	} catch( const pt::ptree_bad_path & badpath ) {
	}
}

void WebQQ::cb_fetch_aid(const boost::system::error_code& ec, read_streamptr stream,  boost::shared_ptr<boost::asio::streambuf> buf, boost::function<void(const boost::system::error_code&, std::string)> handler)
{
	if (!ec)
	{
		// 获取到咯, 更新 verifysession
		detail::update_cookies(&m_cookies, stream->response_options().header_string(), "verifysession");
		m_cookies.update();

		handler(boost::system::error_code(), std::string(boost::asio::buffer_cast<const char*>(buf->data()), boost::asio::buffer_size(buf->data())));
		return;
	}
	handler(ec, std::string());
}

void WebQQ::fetch_aid(std::string arg, boost::function<void(const boost::system::error_code&, std::string)> handler)
{
	std::string url = boost::str(
		boost::format("http://captcha.qq.com/getimage?%s") % arg
	);

	read_streamptr stream(new avhttp::http_stream(m_io_service));
	stream->request_options(
		avhttp::request_opts()
			(avhttp::http_options::referer, "http://web.qq.com/")
			(avhttp::http_options::cookie, m_cookies.lwcookies)
			(avhttp::http_options::connection, "close")
	);

	boost::shared_ptr<boost::asio::streambuf> buffer = boost::make_shared<boost::asio::streambuf>();
	avhttp::async_read_body(*stream, url,*buffer, boost::bind(&WebQQ::cb_fetch_aid, shared_from_this(), _1, stream, buffer, handler ));
}

static void cb_search_group_vcode(const boost::system::error_code& ec, std::string vcodedata, webqq::search_group_handler handler, qqGroup_ptr group)
{
	if (!ec){
		handler(group, 1, vcodedata);
	}else{
		group.reset();
		handler(group, 0, vcodedata);
	}
}

void WebQQ::cb_search_group(std::string groupqqnum, const boost::system::error_code& ec, read_streamptr stream,  boost::shared_ptr<boost::asio::streambuf> buf, webqq::search_group_handler handler)
{
	pt::ptree	jsobj;
	std::istream jsondata(buf.get());
	qqGroup_ptr  group;

	if (!ec){
		// 读取 json 格式
		js::read_json(jsondata, jsobj);
		group.reset(new qqGroup);
		group->qqnum = groupqqnum;
		try{
			if(jsobj.get<int>("retcode") == 0){
				group->qqnum = jsobj.get<std::string>("result..GE");
				group->code = jsobj.get<std::string>("result..GEX");
			}else if(jsobj.get<int>("retcode") == 100110){
				// 需要验证码, 先获取验证码图片，然后回调
				fetch_aid(boost::str(boost::format("aid=1003901&%ld") % std::time(NULL)), boost::bind(cb_search_group_vcode, _1, _2, handler, group) );
				return;
			}else if (jsobj.get<int>("retcode")==100102){
				// 验证码错误
				group.reset();
			}
		}catch (...){
			group.reset();
		}
	}
	handler(group, 0, "");
}

void WebQQ::search_group(std::string groupqqnum, std::string vfcode, webqq::search_group_handler handler)
{
	// GET /keycgi/qqweb/group/search.do?pg=1&perpage=10&all=82069263&c1=0&c2=0&c3=0&st=0&vfcode=&type=1&vfwebqq=59b09b83f622d820cd9ee4e04d4f4e4664e6704ee7ac487ce00595f8c539476b49fdcc372e1d11ea&t=1365138435110 HTTP/1.1
	std::string url = boost::str(
		boost::format("%s/keycgi/qqweb/group/search.do?pg=1&perpage=10&all=%s&c1=0&c2=0&c3=0&st=0&vfcode=%s&type=1&vfwebqq=%s&t=%ld")
			%  "http://cgi.web2.qq.com" % groupqqnum % vfcode %  m_vfwebqq % std::time(NULL)
	);

	read_streamptr stream(new avhttp::http_stream(m_io_service));
	stream->request_options(avhttp::request_opts()
		(avhttp::http_options::content_type, "utf-8")
		(avhttp::http_options::referer, "http://cgi.web2.qq.com/proxy.html?v=201304220930&callback=1&id=2")
		(avhttp::http_options::cookie, m_cookies.lwcookies)
		(avhttp::http_options::connection, "close")
	);

	boost::shared_ptr<boost::asio::streambuf> buffer = boost::make_shared<boost::asio::streambuf>();
	avhttp::async_read_body(*stream, url, * buffer, boost::bind(&WebQQ::cb_search_group, shared_from_this(), groupqqnum, _1, stream, buffer, handler));
}

static void cb_join_group_vcode(const boost::system::error_code& ec, std::string vcodedata, webqq::join_group_handler handler, qqGroup_ptr group)
{
	if (!ec){
		handler(group, 1, vcodedata);
	}else{
		group.reset();
		handler(group, 0, vcodedata);
	}
}


void WebQQ::cb_join_group( qqGroup_ptr group, const boost::system::error_code& ec, read_streamptr stream, boost::shared_ptr<boost::asio::streambuf> buf, webqq::join_group_handler handler )
{
	// 检查返回值是不是 retcode == 0
	pt::ptree	jsobj;
	std::istream jsondata(buf.get());
	try{
		js::read_json(jsondata, jsobj);
		js::write_json(std::cerr, jsobj);

		if(jsobj.get<int>("retcode") == 0){
			// 搞定！群加入咯. 等待管理员吧.
			handler(group, 0, "");
			// 获取群的其他信息
			// GET http://s.web2.qq.com/api/get_group_public_info2?gcode=3272859045&vfwebqq=f08e7a200fd0be375d753d3fedfd24e99f6ba0a8063005030bb95f9fa4b7e0c30415ae74e77709e3&t=1365161430494 HTTP/1.1
		}else if(jsobj.get<int>("retcode") == 100001){
			std::cout << console_out_str("原因： ") <<   jsobj.get<std::string>("tips") <<  std::endl;
			// 需要验证码, 先获取验证码图片，然后回调
			fetch_aid(boost::str(boost::format("aid=%s&_=%ld") % APPID % std::time(NULL)), boost::bind(cb_join_group_vcode, _1, _2, handler, group) );
		}else{
			// 需要验证码, 先获取验证码图片，然后回调
			fetch_aid(boost::str(boost::format("aid=%s&_=%ld") % APPID % std::time(NULL)), boost::bind(cb_join_group_vcode, _1, _2, handler, group) );
		}
	}catch (...){
		handler(qqGroup_ptr(), 0, "");
	}
}


void WebQQ::join_group(qqGroup_ptr group, std::string vfcode, webqq::join_group_handler handler )
{
	std::string url = "http://s.web2.qq.com/api/apply_join_group2";
	
	std::string postdata =	boost::str(
								boost::format(
									"{\"gcode\":%s,"
									"\"code\":\"%s\","
									"\"vfy\":\"%s\","
									"\"msg\":\"avbot\","
									"\"vfwebqq\":\"%s\"}" )
								% group->code % vfcode % m_cookies.verifysession % m_vfwebqq
							);

	postdata = std::string("r=") + boost::url_encode(postdata);

	read_streamptr stream(new avhttp::http_stream(m_io_service));
	stream->request_options(avhttp::request_opts()
		(avhttp::http_options::http_version, "HTTP/1.0")
		(avhttp::http_options::content_type, "application/x-www-form-urlencoded; charset=UTF-8")
		(avhttp::http_options::referer, "http://s.web2.qq.com/proxy.html?v=201304220930&callback=1&id=1")
		(avhttp::http_options::cookie, m_cookies.lwcookies)
		(avhttp::http_options::connection, "close")
		(avhttp::http_options::request_method, "POST")
		(avhttp::http_options::request_body, postdata)
		(avhttp::http_options::content_length, boost::lexical_cast<std::string>(postdata.length()))
	);

	boost::shared_ptr<boost::asio::streambuf> buffer = boost::make_shared<boost::asio::streambuf>();

	avhttp::async_read_body(*stream, url, * buffer, boost::bind(&WebQQ::cb_join_group, shared_from_this(), group, _1, stream, buffer, handler));
}

// 把 boost::u8_to_u32_iterator 封装一下，提供 * 解引用操作符.
class u8_u32_iterator: public boost::u8_to_u32_iterator<std::string::const_iterator>
{
public:
	typedef boost::uint32_t reference;

	reference operator* () const
	{
		return dereference();
	}
	u8_u32_iterator( std::string::const_iterator b ):
		boost::u8_to_u32_iterator<std::string::const_iterator>( b ) {}
};

// 向后迭代，然后返回每个字符
template<class BaseIterator>
struct escape_iterator
{
	BaseIterator m_position;
	typedef std::string reference;

	escape_iterator( BaseIterator b ): m_position( b ) {}

	void operator ++()
	{
		++m_position;
	}

	void operator ++(int)
	{
		++m_position;
	}

	reference operator* () const
	{
		char buf[8] = {0};

		snprintf( buf, sizeof( buf ), "\\\\u%04X", ( boost::uint32_t )( * m_position ) );
		// 好，解引用！
		// 获得 代码点后，就是构造  \\\\uXXXX 了
		return buf;
	}

	bool operator == ( const escape_iterator & rhs ) const
	{
		return m_position == rhs.m_position;
	}

	bool operator != ( const escape_iterator & rhs ) const
	{
		return m_position != rhs.m_position;
	}
};

static std::string parse_unescape( const std::string & source )
{
	std::string result;
	escape_iterator<u8_u32_iterator> ues( source.begin() );
	escape_iterator<u8_u32_iterator> end( source.end() );
	try{
		while( ues != end )
		{
			result += * ues;
			++ ues;
		}
	}catch (const std::out_of_range &e)
	{
		BOOST_LOG_TRIVIAL(error) << __FILE__ <<  __LINE__<<  " "  <<  console_out_str("QQ消息字符串包含非法字符 ");
	}

	return result;
}
