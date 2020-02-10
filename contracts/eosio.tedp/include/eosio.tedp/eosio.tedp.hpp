#include <eosio/eosio.hpp>
#include <eosio/system.hpp>
#include <eosio/asset.hpp>

using namespace std;
using namespace eosio;

class[[eosio::contract("eosio.tedp")]] tedp : public contract
{
public:
   using contract::contract;
   tedp(name receiver, name code, datastream<const char *> ds)
       : contract(receiver, code, ds), payouts(receiver, receiver.value),
         rexbuys(receiver, receiver.value) {}

   ACTION settf(uint64_t amount);
   ACTION setecondev(uint64_t amount);
   ACTION setrex(uint64_t amount);
   ACTION delpayout(name to);
   ACTION pay();
   ACTION setrexbuy(name account_name, uint64_t buy_time, uint64_t interval, asset cpu_amount, asset net_amount);
   ACTION cancelrexbuy(name account_name);

private:
   void setpayout(name to, uint64_t amount, uint64_t interval);
   asset do_rexbuy(asset total_payout);
   void rexdeposit(asset deposit_amount);
   void rexnet(asset net_amount, name for_account);
   void rexcpu(asset cpu_amount, name for_account);

   TABLE payout
   {
      name to;
      uint64_t amount;
      uint64_t interval;
      uint64_t last_payout;
      uint64_t primary_key() const { return to.value; }
   };

   typedef multi_index<name("payouts"), payout> payout_table;

   TABLE rexbuy
   {
      name receiver;
      bool in_progress;
      uint64_t interval;
      uint64_t buy_time;
      asset net_amount;
      asset cpu_amount;
      asset net_left;
      asset cpu_left;
      uint64_t get_secondary_1() const { return buy_time; }
      uint64_t primary_key() const { return receiver.value; }
   };

   typedef multi_index<name("rexbuys"), rexbuy, indexed_by<"buytime"_n, const_mem_fun<rexbuy, uint64_t, &rexbuy::get_secondary_1>>> rexbuy_table;

   using setpayout_action = action_wrapper<name("setpayout"), &tedp::setpayout>;
   using delpayout_action = action_wrapper<name("delpayout"), &tedp::delpayout>;
   using pay_action = action_wrapper<name("payout"), &tedp::pay>;
   payout_table payouts;
   rexbuy_table rexbuys;
};