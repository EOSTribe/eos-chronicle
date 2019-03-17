// copyright defined in LICENSE.txt

#include "receiver_plugin.hpp"
#include <chainbase/chainbase.hpp>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/member.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/iostreams/device/back_inserter.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/program_options.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <memory>
#include <string_view>
#include <fc/log/logger.hpp>
#include <fc/exception/exception.hpp>

using namespace abieos;
using namespace appbase;
using namespace std::literals;
using namespace chain_state;

using std::enable_shared_from_this;
using std::exception;
using std::make_shared;
using std::make_unique;
using std::map;
using std::set;
using std::max;
using std::min;
using std::optional;
using std::runtime_error;
using std::shared_ptr;
using std::string;
using std::string_view;
using std::to_string;
using std::variant;
using std::vector;

namespace asio      = boost::asio;
namespace bio       = boost::iostreams;
namespace bpo       = boost::program_options;
namespace websocket = boost::beast::websocket;

using asio::ip::tcp;
using boost::beast::flat_buffer;
using boost::system::error_code;

namespace {
  const char* RCV_HOST_OPT = "host";
  const char* RCV_PORT_OPT = "port";
  const char* RCV_DBSIZE_OPT = "receiver-state-db-size";
  const char* RCV_EVERY_OPT = "report-every";
  const char* RCV_MAX_QUEUE_OPT = "max-queue-size";
}


// decoder state database objects

namespace chronicle {
  using namespace chainbase;
  using namespace boost::multi_index;
  
  enum dbtables {
    state_table,
    received_blocks_table,
    contract_abi_objects_table,
    table_id_object_table
  };

  struct by_id;
  struct by_blocknum;
  struct by_name;
  struct by_tid;
  
  // this is a singleton keeping the state of the receiver
  
  struct state_object : public chainbase::object<state_table, state_object>  {
    CHAINBASE_DEFAULT_CONSTRUCTOR(state_object);
    id_type     id;
    uint32_t    head;
    checksum256 head_id;
    uint32_t    irreversible;
    checksum256 irreversible_id;
  };
  
  using state_index = chainbase::shared_multi_index_container<
    state_object,
    indexed_by<
      ordered_unique<tag<by_id>, member<state_object, state_object::id_type, &state_object::id>>>>;

  // list of received blocks and their IDs, truncated from head as new blocks are received
  
  struct received_block_object : public chainbase::object<received_blocks_table, received_block_object>  {
    CHAINBASE_DEFAULT_CONSTRUCTOR(received_block_object);
    id_type      id;
    uint32_t     block_index;
    checksum256  block_id;    
  };

  using received_block_index = chainbase::shared_multi_index_container<
    received_block_object,
    indexed_by<
      ordered_unique<tag<by_id>, member<received_block_object,
                                        received_block_object::id_type, &received_block_object::id>>,
      ordered_unique<tag<by_blocknum>, BOOST_MULTI_INDEX_MEMBER(received_block_object, uint32_t, block_index)>>>;

  // serialized binary ABI for every contract

  struct contract_abi_object : public chainbase::object<contract_abi_objects_table, contract_abi_object> {
    template<typename Constructor, typename Allocator>
    contract_abi_object( Constructor&& c, Allocator&& a ) : abi(a) { c(*this); }
    id_type                   id;
    uint64_t                  account;
    chainbase::shared_string  abi;

    void set_abi(const std::vector<char> data) {
      abi.resize(data.size());
      abi.assign(data.data(), data.size());
    }
  };

  using contract_abi_index = chainbase::shared_multi_index_container<
    contract_abi_object,
    indexed_by<
      ordered_unique<tag<by_id>, member<contract_abi_object,
                                        contract_abi_object::id_type, &contract_abi_object::id>>,
      ordered_unique<tag<by_name>, BOOST_MULTI_INDEX_MEMBER(contract_abi_object, uint64_t, account)>>>;
}

CHAINBASE_SET_INDEX_TYPE(chronicle::state_object, chronicle::state_index)
CHAINBASE_SET_INDEX_TYPE(chronicle::received_block_object, chronicle::received_block_index)
CHAINBASE_SET_INDEX_TYPE(chronicle::contract_abi_object, chronicle::contract_abi_index)


std::vector<char> zlib_decompress(input_buffer data) {
  std::vector<char>      out;
  bio::filtering_ostream decomp;
  decomp.push(bio::zlib_decompressor());
  decomp.push(bio::back_inserter(out));
  bio::write(decomp, data.pos, data.end - data.pos);
  bio::close(decomp);
  return out;
}





class receiver_plugin_impl : std::enable_shared_from_this<receiver_plugin_impl> {
public:
  receiver_plugin_impl() :
    _forks_chan(app().get_channel<chronicle::channels::forks>()),
    _blocks_chan(app().get_channel<chronicle::channels::blocks>()),
    _block_table_deltas_chan(app().get_channel<chronicle::channels::block_table_deltas>()),
    _transaction_traces_chan(app().get_channel<chronicle::channels::transaction_traces>()),
    _abi_updates_chan(app().get_channel<chronicle::channels::abi_updates>()),
    _abi_removals_chan(app().get_channel<chronicle::channels::abi_removals>()),
    _abi_errors_chan(app().get_channel<chronicle::channels::abi_errors>()),
    _table_row_updates_chan(app().get_channel<chronicle::channels::table_row_updates>()),
    _receiver_pauses_chan(app().get_channel<chronicle::channels::receiver_pauses>()),
    mytimer(std::ref(app().get_io_service()))
  {};
  
  shared_ptr<chainbase::database>       db;
  shared_ptr<tcp::resolver>             resolver;
  shared_ptr<websocket::stream<tcp::socket>> stream;
  string                                host;
  string                                port;
  uint32_t                              report_every = 0;
  uint32_t                              max_queue_size = 0;
  bool                                  aborting = false;
  
  uint32_t                              head            = 0;
  checksum256                           head_id         = {};
  uint32_t                              irreversible    = 0;
  checksum256                           irreversible_id = {};
  uint32_t                              first_bulk      = 0;
  abieos::block_timestamp               block_timestamp;
  
  // needed for decoding state history input
  map<string, abi_type>                 abi_types;

  // The context keeps decoded versions of contract ABI
  abieos_context*                       contract_abi_ctxt = nullptr;
  set<uint64_t>                         contract_abi_imported;

  std::map<name,std::set<name>>         blacklist_actions;

  chronicle::channels::forks::channel_type&               _forks_chan;
  chronicle::channels::blocks::channel_type&              _blocks_chan;
  chronicle::channels::block_table_deltas::channel_type&  _block_table_deltas_chan;
  chronicle::channels::transaction_traces::channel_type&  _transaction_traces_chan;
  chronicle::channels::abi_updates::channel_type&         _abi_updates_chan;
  chronicle::channels::abi_removals::channel_type&        _abi_removals_chan;
  chronicle::channels::abi_errors::channel_type&          _abi_errors_chan;
  chronicle::channels::table_row_updates::channel_type&   _table_row_updates_chan;
  chronicle::channels::receiver_pauses::channel_type&     _receiver_pauses_chan;

  const int channel_priority = 50;

  bool                                  exporter_will_ack = false;
  uint32_t                              exporter_acked_block = 0;
  uint32_t                              exporter_max_unconfirmed;
  boost::asio::deadline_timer           mytimer;
  uint32_t                              pause_time_msec = 0;
  bool                                  slowdown_requested = false;

  
  void start() {
    load_state();
    resolver->async_resolve
      (host, port,
       [this](const error_code ec, tcp::resolver::results_type results) {
        if (ec)
          elog("Error during lookup of ${h}:${p} - ${e}", ("h",host)("p",port)("e", ec.message()));
        
        callback(ec, "resolve", [&] {
            asio::async_connect
              (
               stream->next_layer(),
               results.begin(),
               results.end(),
               [this](const error_code ec, auto&) {
                 callback(ec, "connect", [&] {
                     stream->async_handshake(host, "/", [this](const error_code ec) {
                         callback(ec, "handshake", [&] {
                             start_read();
                           });
                       });
                   });
               });
          });
      });
  }

  
  void load_state() {
    bool did_undo = false;
    uint32_t depth;
    {
      auto &index = db->get_index<chronicle::state_index>();
      if( index.stack().size() > 0 ) {
        depth = index.stack().size();
        ilog("Database has ${d} uncommitted revisions. Reverting back", ("d",depth));
        while (index.stack().size() > 0)
          db->undo();
        did_undo = true;
      }
    }

    const auto& idx = db->get_index<chronicle::state_index, chronicle::by_id>();
    auto itr = idx.begin();
    if( itr != idx.end() ) {
      head = itr->head;
      head_id = itr->head_id;
      irreversible = itr->irreversible;
      irreversible_id = itr->irreversible_id;
    }

    if( did_undo ) {
      ilog("Reverted to block=${b}, issuing an explicit fork event", ("b",head));
      auto fe = std::make_shared<chronicle::channels::fork_event>();
      fe->fork_block_num = head;
      fe->depth = depth;
      fe->fork_reason = chronicle::channels::fork_reason_val::restart;
      _forks_chan.publish(channel_priority, fe);
    }
    
    if( exporter_will_ack )
      exporter_acked_block = head;

    init_contract_abi_ctxt();
  }

  
  
  void save_state() {
    const auto& idx = db->get_index<chronicle::state_index, chronicle::by_id>();
    auto itr = idx.begin();
    if( itr != idx.end() ) {
      db->modify( *itr, [&]( chronicle::state_object& o ) {
          o.head = head;
          o.head_id = head_id;
          o.irreversible = irreversible;
          o.irreversible_id = irreversible_id;
        });
    }
    else {
      db->create<chronicle::state_object>( [&]( chronicle::state_object& o ) {
          o.head = head;
          o.head_id = head_id;
          o.irreversible = irreversible;
          o.irreversible_id = irreversible_id;
        });
    }
  }
  
  
  void start_read() {
    auto in_buffer = make_shared<flat_buffer>();
    stream->async_read(*in_buffer, [this, in_buffer](const error_code ec, size_t) {
        callback(ec, "async_read", [&] {
            receive_abi(in_buffer);
            request_blocks();
            continue_read();
          });
      });
  }

  
  void continue_read() {
    if( check_pause() ) {
      pause_time_msec = 0;
      auto in_buffer = make_shared<flat_buffer>();
      stream->async_read(*in_buffer, [this, in_buffer](const error_code ec, size_t) {
          callback(ec, "async_read", [&] {
              if (!receive_result(in_buffer))
                return;
              continue_read();
            });
        });
    }
  }

  
  // if consumer fails to acknowledge on time, or processing queues get too big, we pacify the receiver
  bool check_pause() {
    if( slowdown_requested || 
        (exporter_will_ack && head - exporter_acked_block >= exporter_max_unconfirmed) ||
        app().get_priority_queue().size() > max_queue_size ) {
      
      slowdown_requested = false;
      
      if( pause_time_msec == 0 ) {
        pause_time_msec = 100;
      }
      else if( pause_time_msec < 8000 ) {
        pause_time_msec *= 2;
      }

      if( pause_time_msec >= 2000 ) {
        auto rp = std::make_shared<chronicle::channels::receiver_pause>();
        rp->head = head;
        rp->acknowledged = exporter_acked_block;
        _receiver_pauses_chan.publish(channel_priority, rp);
        ilog("Pausing the reader");
      }
      
      mytimer.expires_from_now(boost::posix_time::milliseconds(pause_time_msec));
      mytimer.async_wait([this](const error_code ec) {
          callback(ec, "async_wait", [&] {
              continue_read();
            });
        });
      return false;
    }
    return true;
  }
  
  
  void receive_abi(const shared_ptr<flat_buffer>& p) {
    auto data = p->data();
    std::string error;
    abi_def abi{};
    if (!json_to_native(abi, error, string_view{(const char*)data.data(), data.size()}))
      throw runtime_error("abi parse error: " + error);
    if( !check_abi_version(abi.version, error) )
      throw runtime_error("abi version error: " + error);
    abieos::contract c;
    if( !fill_contract(c, error, abi) )
      throw runtime_error(error);
    abi_types = std::move(c.abi_types);
  }

  
  void request_blocks()
  {
    jarray positions;
    const auto& idx = db->get_index<chronicle::received_block_index, chronicle::by_blocknum>();
    auto itr = idx.lower_bound(irreversible);
    while( itr != idx.end() && itr->block_index <= head ) {
      positions.push_back(jvalue{jobject{
            {{"block_num"s}, {std::to_string(itr->block_index)}},
              {{"block_id"s}, {(string)itr->block_id}},
                }});
      itr++;
    }

    uint32_t start_block = head + 1;
    ilog("Start block: ${b}", ("b",start_block));
    
    send(jvalue{jarray{{"get_blocks_request_v0"s},
            {jobject{
                {{"start_block_num"s}, {to_string(start_block)}},
                  {{"end_block_num"s}, {"4294967295"s}},
                    {{"max_messages_in_flight"s}, {"4294967295"s}},
                      {{"have_positions"s}, {positions}},
                        {{"irreversible_only"s}, {false}},
                          {{"fetch_block"s}, {true}},
                            {{"fetch_traces"s}, {true}},
                              {{"fetch_deltas"s}, {true}},
                                }}}});
  }

  
  bool receive_result(const shared_ptr<flat_buffer>& p) {
    auto         data = p->data();
    input_buffer bin{(const char*)data.data(), (const char*)data.data() + data.size()};
    check_variant(bin, get_type("result"), "get_blocks_result_v0");

    string error;
    get_blocks_result_v0 result;
    if (!bin_to_native(result, error, bin))
      throw runtime_error("result conversion error: " + error);

    if (!result.this_block)
      return true;
      
    uint32_t    last_irreversoble_num = result.last_irreversible.block_num;

    uint32_t    block_num = result.this_block->block_num;
    checksum256 block_id = result.this_block->block_id;

    if( db->revision() < block_num ) {
      db->set_revision(block_num);
      dlog("set DB revision to ${r}", ("r",block_num));
    }
      
    if( block_num > last_irreversoble_num ) {
      // we're at the blockchain head
      if (block_num <= head) { //received a block that is lower than what we already saw
        ilog("fork detected at block ${b}; head=${h}", ("b",block_num)("h",head));
        uint32_t depth = head - block_num;
        init_contract_abi_ctxt();
        while( db->revision() >= block_num ) {
          db->undo();
        }
        dlog("rolled back DB revision to ${r}", ("r",db->revision()));
        if( db->revision() <= 0 ) {
          throw runtime_error(std::string("Cannot rollback, no undo stack at revision ")+
                              std::to_string(db->revision()));
        }

        auto fe = std::make_shared<chronicle::channels::fork_event>();
        fe->fork_block_num = block_num;
        fe->depth = depth;
        fe->fork_reason = chronicle::channels::fork_reason_val::network;
        _forks_chan.publish(channel_priority, fe);
      }
      else
        if (head > 0 && (!result.prev_block || result.prev_block->block_id.value != head_id.value))
          throw runtime_error("prev_block does not match");
    }
    
    auto undo_session = db->start_undo_session(true);

    if( block_num > irreversible ) {
      // add the new block
      const auto& idx = db->get_index<chronicle::received_block_index, chronicle::by_blocknum>();
      db->create<chronicle::received_block_object>( [&]( chronicle::received_block_object& o ) {
          o.block_index = block_num;
          o.block_id = block_id;
        });
      // truncate old blocks up to previously known irreversible
      auto itr = idx.begin();
      while( itr->block_index < irreversible && itr != idx.end() ) {
        db->remove(*itr);
        itr = idx.begin();
      }
    }

    head            = block_num;
    head_id         = block_id;
    irreversible    = result.last_irreversible.block_num;
    irreversible_id = result.last_irreversible.block_id;
    
    if (result.block)
      receive_block(*result.block);
    if (result.deltas)
      receive_deltas(*result.deltas);
    if (result.traces)
      receive_traces(*result.traces);

    if( aborting )
      return false;
    
    save_state();
    undo_session.push();     // save a new revision

    // if exporter is acknowledging, we only commit what is confirmed
    auto commit_rev = irreversible;
    if( exporter_will_ack && exporter_acked_block < commit_rev ) {
      commit_rev = exporter_acked_block;
    }
    db->commit(commit_rev);

    return true;
  }

  


  
  void receive_block(input_buffer bin) {
    if( head == irreversible ) {
      ilog("Crossing irreversible block=${h}", ("h",head));
    }
      
    if (report_every > 0 && head % report_every == 0) {
         uint64_t free_bytes = db->get_segment_manager()->get_free_memory();
         uint64_t size = db->get_segment_manager()->get_size();
         ilog("block=${h}; irreversible=${i}; dbmem_free=${m}",
              ("h",head)("i",irreversible)("m", free_bytes*100/size));
         if( exporter_will_ack )
           ilog("Exporter acknowledged block=${b}", ("b", exporter_acked_block));
         ilog("appbase priority queue size: ${q}", ("q", app().get_priority_queue().size()));
    }

    auto block_ptr = std::make_shared<chronicle::channels::block>();
    block_ptr->block_num = head;
    block_ptr->last_irreversible = irreversible;

    string error;
    if (!bin_to_native(block_ptr->block, error, bin))
      throw runtime_error("block conversion error: " + error);
    block_timestamp = block_ptr->block.timestamp;
    _blocks_chan.publish(channel_priority, block_ptr);
  }


  
  void receive_deltas(input_buffer buf) {
    auto         data = zlib_decompress(buf);
    input_buffer bin{data.data(), data.data() + data.size()};
    
    uint32_t num;
    string error;
    if( !read_varuint32(bin, error, num) )
      throw runtime_error(error);
    for (uint32_t i = 0; i < num; ++i) {
      check_variant(bin, get_type("table_delta"), "table_delta_v0");
      
      auto bltd = std::make_shared<chronicle::channels::block_table_delta>();
      bltd->block_timestamp = block_timestamp;

      string error;
      if (!bin_to_native(bltd->table_delta, error, bin))
        throw runtime_error("table_delta conversion error: " + error);
      
      auto& variant_type = get_type(bltd->table_delta.name);
      if (!variant_type.filled_variant || variant_type.fields.size() != 1 || !variant_type.fields[0].type->filled_struct)
        throw std::runtime_error("don't know how to proccess " + variant_type.name);
      auto& type = *variant_type.fields[0].type;

      size_t num_processed = 0;
      for (auto& row : bltd->table_delta.rows) {
        check_variant(row.data, variant_type, 0u);
      }

      if (bltd->table_delta.name == "account") {  // memorize contract ABI
        for (auto& row : bltd->table_delta.rows) {
          if (row.present) {
            string error;
            account_object acc;
            if (!bin_to_native(acc, error, row.data))
              throw runtime_error("account row conversion error: " + error);
            if( acc.abi.data.size() == 0 ) {
              clear_contract_abi(acc.name);
            }
            else {
              save_contract_abi(acc.name, acc.abi.data);
            }
          }
        }
      }
      else {
        if (bltd->table_delta.name == "contract_row" && 
            (_table_row_updates_chan.has_subscribers() || _abi_errors_chan.has_subscribers())) {
          for (auto& row : bltd->table_delta.rows) {
            auto tru = std::make_shared<chronicle::channels::table_row_update>();
            tru->block_num = head;
            tru->block_timestamp = block_timestamp;

            string error;
            if (!bin_to_native(tru->kvo, error, row.data))
              throw runtime_error("cannot read table row object" + error);
            if( get_contract_abi_ready(tru->kvo.code) ) {
              tru->added = row.present;
              _table_row_updates_chan.publish(channel_priority, tru);
            }
            else {
              auto ae =  std::make_shared<chronicle::channels::abi_error>();
              ae->block_num = head;
              ae->block_timestamp = block_timestamp;
              ae->account = tru->kvo.code;
              ae->error = "cannot decode table delta because of missing ABI";
              _abi_errors_chan.publish(channel_priority, ae);
            }
          }
        }
      }
      _block_table_deltas_chan.publish(channel_priority, bltd);
    }
  } // receive_deltas


  void init_contract_abi_ctxt() {
    if( contract_abi_ctxt ) {
      // dlog("Destroying ABI cache");
      abieos_destroy(contract_abi_ctxt);
      contract_abi_imported.clear();
    }
    contract_abi_ctxt = abieos_create();
  }

  
  void clear_contract_abi(name account) {
    if( contract_abi_imported.count(account.value) > 0 )
      init_contract_abi_ctxt(); // abieos_contract does not support removals, so we have to destroy it
    const auto& idx = db->get_index<chronicle::contract_abi_index, chronicle::by_name>();
    auto itr = idx.find(account.value);
    if( itr != idx.end() ) {
      // dlog("Clearing contract ABI for ${a}", ("a",(std::string)account));
      db->remove(*itr);
      
      auto ar =  std::make_shared<chronicle::channels::abi_removal>();
      ar->block_num = head;
      ar->block_timestamp = block_timestamp;
      ar->account = account;
      _abi_removals_chan.publish(channel_priority, ar);
    }
  }


  
  void save_contract_abi(name account, std::vector<char> data) {
    // dlog("Saving contract ABI for ${a}", ("a",(std::string)account));
    if( contract_abi_imported.count(account.value) > 0 ) {
      init_contract_abi_ctxt();
    }

    try {
      // this checks the validity of ABI
      if( !abieos_set_abi_bin(contract_abi_ctxt, account.value, data.data(), data.size()) ) {
        throw runtime_error( abieos_get_error(contract_abi_ctxt) );
      }        
      contract_abi_imported.insert(account.value);
      
      const auto& idx = db->get_index<chronicle::contract_abi_index, chronicle::by_name>();
      auto itr = idx.find(account.value);
      if( itr != idx.end() ) {
        db->modify( *itr, [&]( chronicle::contract_abi_object& o ) {
            o.set_abi(data);
          });
      }
      else {
        db->create<chronicle::contract_abi_object>( [&]( chronicle::contract_abi_object& o ) {
            o.account = account.value;
            o.set_abi(data);
          });
      }

      if (_abi_updates_chan.has_subscribers()) {
        auto abiupd = std::make_shared<chronicle::channels::abi_update>();
        abiupd->block_num = head;
        abiupd->block_timestamp = block_timestamp;
        abiupd->account = account;
        abiupd->abi_bytes = bytes {data};
        input_buffer buf{data.data(), data.data() + data.size()};
        string error;
        if (!bin_to_native(abiupd->abi, error, buf))
          throw runtime_error(error);
        _abi_updates_chan.publish(channel_priority, abiupd);
      }
    }
    catch (const exception& e) {
      wlog("Cannot use ABI for ${a}: ${e}", ("a",(std::string)account)("e",e.what()));
      auto ae = std::make_shared<chronicle::channels::abi_error>();
      ae->block_num = head;
      ae->block_timestamp = block_timestamp;
      ae->account = account;
      ae->error = e.what();
      _abi_errors_chan.publish(channel_priority, ae);
    }
  }

  
  bool get_contract_abi_ready(name account) {
    if( contract_abi_imported.count(account.value) > 0 )
      return true; // the context has this contract loaded
    const auto& idx = db->get_index<chronicle::contract_abi_index, chronicle::by_name>();
    auto itr = idx.find(account.value);
    if( itr != idx.end() ) {
      // dlog("Found in DB: ABI for ${a}", ("a",(std::string)account));
      abieos_set_abi_bin(contract_abi_ctxt, account.value, itr->abi.data(), itr->abi.size());
      contract_abi_imported.insert(account.value);
      return true;
    }
    return false;
  }
  
  
  void receive_traces(input_buffer buf) {
    if (_transaction_traces_chan.has_subscribers()) {
      auto         data = zlib_decompress(buf);
      input_buffer bin{data.data(), data.data() + data.size()};
      uint32_t num;
      string       error;
      if( !read_varuint32(bin, error, num) )
        throw runtime_error(error);
      for (uint32_t i = 0; i < num; ++i) {        
        auto tr = std::make_shared<chronicle::channels::transaction_trace>();
        if (!bin_to_native(tr->trace, error, bin))
          throw runtime_error("transaction_trace conversion error: " + error);
        // check blacklist
        bool blacklisted = false;
        if( tr->trace.traces.size() > 0 ) {
          auto &at = tr->trace.traces[0];
          auto search_acc = blacklist_actions.find(at.account);
          if(search_acc != blacklist_actions.end()) {
            if( search_acc->second.count(at.name) != 0 ) {
              blacklisted = true;
            }
          }
        }
        if( !blacklisted ) {
          tr->block_num = head;
          tr->block_timestamp = block_timestamp;
          _transaction_traces_chan.publish(channel_priority, tr);
        }
      }
    }
  }


  const abi_type& get_type(const string& name) {
    auto it = abi_types.find(name);
    if (it == abi_types.end())
      throw runtime_error("unknown type "s + name);
    return it->second;
  }


  void send(const jvalue& value) {
    string error;
    auto bin = make_shared<vector<char>>();
    if (!json_to_bin(*bin, error, &get_type("request"), value))
      throw runtime_error("failed to convert during send: " + error);

    stream->async_write(asio::buffer(*bin),
                       [bin, this](const error_code ec, size_t) {
                         callback(ec, "async_write", [&] {}); });
  }

  
  void check_variant(input_buffer& bin, const abi_type& type, uint32_t expected) {
    string error;
    uint32_t index;
    if( !read_varuint32(bin, error, index) )
      throw runtime_error(error);
    if (!type.filled_variant)
      throw runtime_error(type.name + " is not a variant"s);
    if (index >= type.fields.size())
      throw runtime_error("expected "s + type.fields[expected].name + " got " + to_string(index));
    if (index != expected)
      throw runtime_error("expected "s + type.fields[expected].name + " got " + type.fields[index].name);
  }

  
  void check_variant(input_buffer& bin, const abi_type& type, const char* expected) {
    string error;
    uint32_t index;
    if( !read_varuint32(bin, error, index) )
      throw runtime_error(error);
    if (!type.filled_variant)
      throw runtime_error(type.name + " is not a variant"s);
    if (index >= type.fields.size())
      throw runtime_error("expected "s + expected + " got " + to_string(index));
    if (type.fields[index].name != expected)
      throw runtime_error("expected "s + expected + " got " + type.fields[index].name);
  }

  
  template <typename F>
  void catch_and_close(F f) {
    try {
      f();
    } catch (const exception& e) {
      elog("ERROR: ${e}", ("e",e.what()));
      close();
    } catch (...) {
      elog("ERROR: unknown exception");
      close();
    }
  }

  
  template <typename F>
  void callback(const error_code ec, const char* what, F f) {
    if (ec)
      return on_fail(ec, what);
    catch_and_close(f);
  }

  void on_fail(const error_code ec, const char* what) {
    try {
      elog("ERROR: ${e}", ("e",ec.message()));
      close();
    } catch (...) {
      elog("ERROR: exception while closing");
    }
  }

  void close() {
    if( stream.use_count() > 0 && stream->is_open() ) {
      stream->next_layer().close();
    }
  }
};



receiver_plugin::receiver_plugin() : my(new receiver_plugin_impl)
{
  assert(receiver_plug == nullptr);
  receiver_plug = this;
};

receiver_plugin::~receiver_plugin(){
};


void receiver_plugin::set_program_options( options_description& cli, options_description& cfg ) {
  cfg.add_options()
    (RCV_HOST_OPT, bpo::value<string>()->default_value("localhost"), "Host to connect to (nodeos)")
    (RCV_PORT_OPT, bpo::value<string>()->default_value("8080"), "Port to connect to (nodeos state-history plugin)")
    (RCV_DBSIZE_OPT, bpo::value<uint32_t>()->default_value(1024), "database size in MB")
    (RCV_EVERY_OPT, bpo::value<uint32_t>()->default_value(10000), "Report current state every N blocks")
    (RCV_MAX_QUEUE_OPT, bpo::value<uint32_t>()->default_value(10000), "Maximum size of appbase priority queue")
    ;
}

  
void receiver_plugin::plugin_initialize( const variables_map& options ) {
  try {    
    if( !options.count("data-dir") ) {
      throw std::runtime_error("--data-dir option is required");
    }
    
    my->db = std::make_shared<chainbase::database>
      (app().data_dir().native() + "/receiver-state",
       chainbase::database::read_write,
       options.at(RCV_DBSIZE_OPT).as<uint32_t>() * 1024*1024);
    my->db->add_index<chronicle::state_index>();
    my->db->add_index<chronicle::received_block_index>();
    my->db->add_index<chronicle::contract_abi_index>();
    
    my->resolver = std::make_shared<tcp::resolver>(std::ref(app().get_io_service()));
    
    my->stream = std::make_shared<websocket::stream<tcp::socket>>(std::ref(app().get_io_service()));
    my->stream->binary(true);
    my->stream->read_message_max(1024 * 1024 * 1024);
    
    my->host = options.at(RCV_HOST_OPT).as<string>();
    my->port = options.at(RCV_PORT_OPT).as<string>();
    my->report_every = options.at(RCV_EVERY_OPT).as<uint32_t>();
    my->max_queue_size = options.at(RCV_MAX_QUEUE_OPT).as<uint32_t>();
    
    my->blacklist_actions.emplace
      (std::make_pair(abieos::name("eosio"),
                      std::set<abieos::name>{abieos::name("onblock")} ));
    my->blacklist_actions.emplace
      (std::make_pair(abieos::name("blocktwitter"),
                      std::set<abieos::name>{abieos::name("tweet")} ));
    
    ilog("Initialized receiver_plugin");
  }
  FC_LOG_AND_RETHROW();
}



void receiver_plugin::start_after_dependencies() {
  bool mustwait = false;
  while( dependent_plugins.size() > 0 && !mustwait ) {
    if( std::get<0>(dependent_plugins[0])->get_state() == appbase::abstract_plugin::started ) {
      ilog("Dependent plugin has started: ${p}", ("p",std::get<1>(dependent_plugins[0])));
      dependent_plugins.erase(dependent_plugins.begin());
    }
    else {
      ilog("Dependent plugin has not yet started: ${p}", ("p",std::get<1>(dependent_plugins[0])));
      mustwait = true;
    }
  }
  
  if( mustwait ) {
    ilog("Waiting for dependent plugins");
    my->mytimer.expires_from_now(boost::posix_time::seconds(1));
    my->mytimer.async_wait(boost::bind(&receiver_plugin::start_after_dependencies, this));
  }
  else {
    ilog("All dependent plugins started, launching the receiver");
    my->start();
  }
};


void receiver_plugin::plugin_startup(){
  start_after_dependencies();
  ilog("Started receiver_plugin");
}


void receiver_plugin::plugin_shutdown() {
  ilog("receiver_plugin stopped");
}


void receiver_plugin::exporter_will_ack_blocks(uint32_t max_unconfirmed) {
  assert(!my->exporter_will_ack);
  assert(max_unconfirmed > 0);
  my->exporter_will_ack = true;
  my->exporter_max_unconfirmed = max_unconfirmed;
  ilog("Receiver will pause at ${u} unacknowledged blocks", ("u", my->exporter_max_unconfirmed));
}

void receiver_plugin::ack_block(uint32_t block_num) {
  assert(my->exporter_will_ack);
  if( block_num < my->exporter_acked_block ) {
    elog("Exporter acked block=${a}, but block=${k} was already acknowledged",
         ("a",block_num)("b",my->exporter_acked_block));
    throw runtime_error("Exporter acked block below prevuously acked one");
  }
  my->exporter_acked_block = block_num;
  //dlog("Acked block=${b}", ("b",block_num));
}

void receiver_plugin::slowdown() {
  my->slowdown_requested = true;
}

abieos_context* receiver_plugin::get_contract_abi_ctxt(abieos::name account) {
  my->get_contract_abi_ready(account);
  return my->contract_abi_ctxt;
}

void receiver_plugin::add_dependency(appbase::abstract_plugin* plug, string plugname) {
  dependent_plugins.emplace_back(std::make_tuple(plug, plugname));
}

void receiver_plugin::abort_receiver() {
  if( my ) {
    my->close();
    my->aborting = true;
  }
}


static bool have_exporter = false;

void exporter_initialized() {
  if( have_exporter )
    throw runtime_error("Only one exporter plugin is allowed");
  have_exporter = true;
}

receiver_plugin* receiver_plug = nullptr;

void exporter_will_ack_blocks(uint32_t max_unconfirmed)
{
  receiver_plug->exporter_will_ack_blocks(max_unconfirmed);
}

// receiver should not start collecting data before all dependent plugins are ready
void donot_start_receiver_before(appbase::abstract_plugin* plug, string plugname) {
  receiver_plug->add_dependency(plug, plugname);
}
  
void abort_receiver() {
  receiver_plug->abort_receiver();
  app().quit();
}


