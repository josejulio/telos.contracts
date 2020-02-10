#include <eosio.tedp/eosio.tedp.hpp>

// tf 1mil
// econdevfunds 500k
// rex 1mil
// wps 1mil
// bpay 1760000
// total 5260000 * 12 momths / 365 days = 172931.5068 per day (63.12mil per year)
// rounding up to ensure contract always can have a surplus beyond the total max of all payouts
// const uint64_t max_drawdown_amount = 173000;

// 60sec * 60min * 24hrs
const uint64_t daily_interval = 86400;

// 60sec * 30min
const uint64_t rex_interval = 1800;

// 500k * 12 months / 365 days = 16438.3561
const uint64_t max_econdev_amount = 16438;

// 1mil * 12 months / 365 days = 32876.7123
const uint64_t max_tf_amount = 32876;

// 1mil * 12 months / 365 days / 24hrs / 2 (every half hour) = 684.9315
const uint64_t max_rex_amount = 685;

ACTION tedp::settf(uint64_t amount)
{
    check(amount <= max_tf_amount, "Max amount for tf account is 32876 per day");
    setpayout(name("tf"), amount, daily_interval);
}

ACTION tedp::setecondev(uint64_t amount)
{
    check(amount <= max_econdev_amount, "Max amount for econdevfunds account is 16438 per day");
    setpayout(name("econdevfunds"), amount, daily_interval);
}

ACTION tedp::setrex(uint64_t amount)
{
    check(amount <= max_rex_amount, "Max amount for eosio.rex account is 685 per 30min");
    setpayout(name("eosio.rex"), amount, rex_interval);
}

void tedp::setpayout(name to, uint64_t amount, uint64_t interval)
{
    require_auth(name("eosio"));
    check(is_account(to), "The payee is not a valid account");
    auto itr = payouts.find(to.value);
    if (itr == payouts.end())
    {
        payouts.emplace(get_self(), [&](auto &p) {
            p.to = to;
            p.amount = amount;
            p.interval = interval;
            p.last_payout = current_time_point().sec_since_epoch();
        });
    }
    else
    {
        payouts.modify(itr, get_self(), [&](auto &p) {
            p.amount = amount;
            p.interval = interval;
        });
    }
}

ACTION tedp::delpayout(name to)
{
    require_auth(name("eosio"));
    auto itr = payouts.find(to.value);
    check(itr != payouts.end(), "Payout does not exist, can't delete");
    payouts.erase(itr);
}

ACTION tedp::pay()
{
    //uint64_t now_ms = eosio::current_time_point().sec_since_epoch();
    uint64_t now_ms = current_time_point().sec_since_epoch();
    bool payouts_made = false;
    for (auto itr = payouts.begin(); itr != payouts.end(); itr++)
    {
        auto p = *itr;
        uint64_t time_since_last_payout = now_ms - p.last_payout;
        eosio::print("now_ms:", now_ms, " - p.last_payout:", p.last_payout, " = time_since_last_payout:", time_since_last_payout, "\n");

        uint64_t payouts_due = time_since_last_payout / p.interval;
        eosio::print("time_since_last_payout:", time_since_last_payout, " / p.interval:", p.interval, " = payments_due:", payouts_due, "\n");

        if (payouts_due == 0)
            continue;

        payouts_made = true;
        payouts.modify(itr, get_self(), [&](auto &p) {
            p.last_payout = now_ms;
        });
        uint64_t total_due = payouts_due * p.amount;
        eosio::print("payout ", payouts_due, " payouts of ", p.amount, " TLOS to: ", p.to, " ", p.amount, " TLOS with time: ", now_ms, "\n");
        asset total_payout = asset(total_due * 10000, symbol("TLOS", 4));

        if (p.to == name("eosio.rex"))
        {
            total_payout = do_rexbuy(total_payout);
            // channel_to_rex
            if (total_payout.amount > 0)
            {
                eosio::print("Channeling to rex\n");
                action(permission_level{_self, name("active")}, name("eosio"), name("distviarex"), make_tuple(get_self(), total_payout)).send();
            }
        }
        else
        {
            // transfer
            eosio::print("Transferring\n");
            action(permission_level{_self, name("active")}, name("eosio.token"), name("transfer"), make_tuple(get_self(), p.to, total_payout, std::string("TEDP Funding"))).send();
        }
    }

    check(payouts_made, "No payouts are due");
}

ACTION tedp::setrexbuy(name account_name, uint64_t buy_time, uint64_t interval, asset cpu_amount, asset net_amount)
{
    auto rexbuy = rexbuys.find(account_name.value);
    check(rexbuy == rexbuys.end(), "rexbuy already exists for account");
    rexbuys.emplace(get_self(), [&](auto &b) {
        b.receiver = account_name;
        b.in_progress = false;
        b.interval = interval;
        b.buy_time = buy_time;
        b.net_amount = net_amount;
        b.cpu_amount = cpu_amount;
    });
}

ACTION tedp::cancelrexbuy(name account_name)
{
    auto rexbuy = rexbuys.find(account_name.value);
    check(rexbuy != rexbuys.end(), "rexbuy already exists for account");
    rexbuys.erase(rexbuy);
}

asset tedp::do_rexbuy(asset total_payout)
{
    uint64_t now_sec = current_time_point().sec_since_epoch();
    auto buytime_index = rexbuys.get_index<"buytime"_n>();

    // first pass, handle in progress payments
    for (auto itr = buytime_index.begin(); itr != buytime_index.end(); itr++)
    {
        if (total_payout.amount <= 0)
            break;

        auto buy = *itr;

        if (!buy.in_progress || buy.buy_time > now_sec)
            continue;

        asset total_due = buy.net_left + buy.cpu_left;
        if (total_due <= total_payout)
        {
            total_payout -= total_due;

            buytime_index.modify(itr, get_self(), [&](auto &b) {
                b.in_progress = false;
                b.buy_time = buy.buy_time + buy.interval;
                b.net_left = asset(0, symbol("TLOS", 4));
                b.cpu_left = asset(0, symbol("TLOS", 4));
            });

            // double check in case any math has caused a overflow
            check(total_due <= total_payout, "Trying to buy more rex than total_payout");
            rexdeposit(total_due);
            rexnet(buy.net_left, buy.receiver);
            rexcpu(buy.cpu_left, buy.receiver);
        }
        else
        {
            total_payout -= total_due;

            asset due_split = total_due / 2;
            asset to_cpu = due_split;
            asset to_net = due_split;

            if (buy.net_left < due_split)
            {
                to_net = buy.net_left;
                to_cpu += (due_split - buy.net_left);
            }
            else if (buy.cpu_left < due_split)
            {
                to_cpu = buy.cpu_left;
                to_net += (due_split - buy.cpu_left);
            }

            buytime_index.modify(itr, get_self(), [&](auto &b) {
                b.net_left -= to_net;
                b.cpu_left -= to_cpu;
            });

            // double check in case any math has caused a overflow
            check(total_due <= total_payout, "Trying to buy more rex than total_payout");
            rexdeposit(total_due);
            rexnet(buy.net_left, buy.receiver);
            rexcpu(buy.cpu_left, buy.receiver);
        }
    }

    // second pass, handle new payments
    for (auto itr = buytime_index.begin(); itr != buytime_index.end(); itr++)
    {
        if (total_payout.amount <= 0)
            break;

        auto buy = *itr;
        if (buy.in_progress || buy.buy_time > now_sec)
            continue;

        asset total_due = buy.net_left + buy.cpu_left;
        if (total_due <= total_payout)
        {
            total_payout -= total_due;

            buytime_index.modify(itr, get_self(), [&](auto &b) {
                b.buy_time = buy.buy_time + buy.interval;
                b.net_left = asset(0, symbol("TLOS", 4));
                b.cpu_left = asset(0, symbol("TLOS", 4));
            });

            // double check in case any math has caused a overflow
            check(total_due <= total_payout, "Trying to buy more rex than total_payout");
            rexdeposit(total_due);
            rexnet(buy.net_left, buy.receiver);
            rexcpu(buy.cpu_left, buy.receiver);
        }
        else
        {
            total_payout -= total_due;

            asset due_split = total_due / 2;
            asset to_cpu = due_split;
            asset to_net = due_split;

            if (buy.net_left < due_split)
            {
                to_net = buy.net_left;
                to_cpu += (due_split - buy.net_left);
            }
            else if (buy.cpu_left < due_split)
            {
                to_cpu = buy.cpu_left;
                to_net += (due_split - buy.cpu_left);
            }

            buytime_index.modify(itr, get_self(), [&](auto &b) {
                b.in_progress = true;
                b.net_left -= to_net;
                b.cpu_left -= to_cpu;
            });

            // double check in case any math has caused a overflow
            check(total_due <= total_payout, "Trying to buy more rex than total_payout");
            rexdeposit(total_due);
            rexnet(buy.net_left, buy.receiver);
            rexcpu(buy.cpu_left, buy.receiver);
        }
    }

    return total_payout;
}

void tedp::rexdeposit(asset deposit_amount)
{
    action(
        permission_level{_self, name("active")},
        name("eosio"),
        name("deposit"),
        make_tuple(
            get_self(),
            deposit_amount))
        .send();
}

void tedp::rexnet(asset net_amount, name for_account)
{
    action(
        permission_level{_self, name("active")},
        name("eosio"),
        name("rentnet"),
        make_tuple(
            get_self(),
            for_account,
            net_amount,
            asset(0, symbol("TLOS", 4))))
        .send();
}

void tedp::rexcpu(asset cpu_amount, name for_account)
{
    action(
        permission_level{_self, name("active")},
        name("eosio"),
        name("rentcpu"),
        make_tuple(
            get_self(),
            for_account,
            cpu_amount,
            asset(0, symbol("TLOS", 4))))
        .send();
}