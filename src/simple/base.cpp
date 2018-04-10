/* Copyright (c) 2017-2018, Hans Erik Thrane */

#include "simple/base.h"

#include <roq/logging.h>
#include <roq/stream.h>

#include <gflags/gflags.h>

#include <algorithm>

DEFINE_string(ioc_open, "ioc_open", "Order template.");
DEFINE_string(ioc_close, "ioc_close", "Order template.");

DEFINE_bool(real_trading, false, "Real trading? (Meaning: Send orders).");

namespace examples {
namespace simple {

const char *TRADER = "Trader";  // FIXME(thraneh): introduce an enum for this!

BaseStrategy::BaseStrategy(
    roq::Strategy::Dispatcher& dispatcher,
    const std::string& exchange,
    const std::string& instrument,
    const std::string& gateway)
    : _exchange(exchange),
      _instrument(instrument),
      _gateway(gateway),
      _ioc_open(FLAGS_ioc_open),
      _ioc_close(FLAGS_ioc_close),
      _dispatcher(dispatcher) {
  LOG(INFO) << "real_trading=" << (FLAGS_real_trading ? "true" : "false");
}

// event handlers

void BaseStrategy::on(const roq::TimerEvent&) {
  // TODO(thraneh): check time-out
}

// download

void BaseStrategy::on(const roq::DownloadBeginEvent& event) {
  // raise the download flag to block order management
  _download = true;
  LOG(INFO) << "download=" << (_download ? "true" : "false");
  // reset all variables tracking order management state
  _order_manager_ready = false;
  _market_open = false;
  _long_position_sod = 0.0;
  _short_position_sod = 0.0;
  _long_position_new = 0.0;
  _short_position_new = 0.0;
  _order_traded_quantity.clear();
}

void BaseStrategy::on(const roq::DownloadEndEvent& event) {
  const auto& download_end = event.download_end;
  // the gateway keeps track of order id's (they can't be recycled)
  auto max_order_id = std::max(_max_order_id, download_end.max_order_id);
  if (_max_order_id != max_order_id) {
    _max_order_id = max_order_id;
    LOG(INFO) << "max_order_id=" << _max_order_id;
  }
  // reset the download flag to allow order management
  _download = false;
  LOG(INFO) << "download=" << (_download ? "true" : "false");
}

// batch

void BaseStrategy::on(const roq::BatchBeginEvent&) {
  _market_data_dirty = false;
}

void BaseStrategy::on(const roq::BatchEndEvent&) {
  if (_market_data_dirty)
    update(_market_data);
}

// either

void BaseStrategy::on(const roq::GatewayStatusEvent& event) {
  const auto& gateway_status = event.gateway_status;
  LOG(INFO) << "gateway_status=" << gateway_status;
  // return early if it's not the gateway's order management status
  if (std::strcmp(TRADER, gateway_status.name) != 0)
    return;
  // ready?
  auto order_manager_ready = gateway_status.status == roq::GatewayState::Ready;
  if (_order_manager_ready != order_manager_ready) {
    _order_manager_ready = order_manager_ready;
    LOG(INFO) << "order_manager_ready=" << (_order_manager_ready ? "true" : "false");
  }
}

// order manager

void BaseStrategy::on(const roq::ReferenceDataEvent& event) {
  const auto& reference_data = event.reference_data;
  // return early if it's not the instrument we want to trade
  if (filter(reference_data.exchange, reference_data.instrument))
    return;
  LOG(INFO) << "reference_data=" << reference_data;
  // instrument's tick size
  auto tick_size = reference_data.tick_size;
  if (_tick_size != tick_size && tick_size != 0.0) {
    _tick_size = tick_size;
    LOG(INFO) << "tick_size=" << _tick_size;
  }
}

void BaseStrategy::on(const roq::MarketStatusEvent& event) {
  const auto& market_status = event.market_status;
  // return early if it's not the instrument we want to trade
  if (filter(market_status.exchange, market_status.instrument))
    return;
  LOG(INFO) << "market_status=" << market_status;
  // instrument's trading status
  auto market_open = market_status.trading_status == roq::TradingStatus::Open;
  if (_market_open != market_open) {
    _market_open = market_open;
    LOG(INFO) << "market_open=" << (_market_open ? "true" : "false");
  }
}

// Note! Position updates are only sent during the download phase.
void BaseStrategy::on(const roq::PositionUpdateEvent& event) {
  LOG(INFO) << event;
  const auto& position_update = event.position_update;
  // return early if it's not the instrument we want to trade
  if (filter(position_update.exchange, position_update.instrument))
    return;
  LOG(INFO) << "position_update=" << position_update;
  // initialize start of day position using yesterday's close position
  // note! this is an example-choice, we could also have configured this.
  switch (position_update.trade_direction) {
    case roq::TradeDirection::Buy: {
      _long_position_sod = position_update.position_yesterday;
      LOG(INFO) << "long_position_sod=" << _long_position_sod;
      break;
    }
    case roq::TradeDirection::Sell: {
      _short_position_sod = position_update.position_yesterday;
      LOG(INFO) << "short_position_sod=" << _short_position_sod;
      break;
    }
    default: {
      LOG(FATAL) << "Unexpected";
    }
  }
}

// Note! Order updates may be sent live or during the download phase.
// During the download phase we will receive everything previously
// sent to the gateway. Thus, by managing reconnection and download
// events, we're able to recover the state at which we left off if
// either the gateway restarts (or reconnects) or the client for
// whatever reason has to be restarted.
void BaseStrategy::on(const roq::OrderUpdateEvent& event) {
  LOG(INFO) << event;
  const auto& order_update = event.order_update;
  // return early if it's not the instrument we want to trade
  if (filter(order_update.exchange, order_update.instrument))
    return;
  LOG(INFO) << "order_update=" << order_update;
  // ensure we never recycle order id's
  _max_order_id = std::max(_max_order_id, order_update.order_id);
  // determine if the intention was to open or close
  auto open = parse_open_close(order_update.order_template);
  // compute fill quantity (and keep track of total traded quantity)
  auto& previous = _order_traded_quantity[order_update.order_id];
  auto fill_quantity = std::max(0.0, order_update.traded_quantity - previous);
  previous = order_update.traded_quantity;
  // update positions for new activity
  switch (order_update.trade_direction) {
    case roq::TradeDirection::Buy: {
      _long_position_new += fill_quantity;
      LOG(INFO) << "long_position_new=" << _long_position_new;
      break;
    }
    case roq::TradeDirection::Sell: {
      _short_position_new += fill_quantity;
      LOG(INFO) << "short_position_new=" << _short_position_new;
      break;
    }
    default: {
      LOG(FATAL) << "Unexpected";
    }
  }
}

// request-response

void BaseStrategy::on(const roq::CreateOrderAckEvent& event) {
  LOG_IF(FATAL, _download) << "Unexpected";
  const auto& create_order_ack = event.create_order_ack;
  LOG_IF(INFO, create_order_ack.failure == false) <<
      "create_order_ack=" << create_order_ack;
  LOG_IF(WARNING, create_order_ack.failure) <<
      "create_order_ack=" << create_order_ack;
}

void BaseStrategy::on(const roq::ModifyOrderAckEvent& event) {
  LOG_IF(FATAL, _download) << "Unexpected";
  const auto& modify_order_ack = event.modify_order_ack;
  LOG_IF(INFO, modify_order_ack.failure == false) <<
      "modify_order_ack=" << modify_order_ack;
  LOG_IF(WARNING, modify_order_ack.failure) <<
      "modify_order_ack=" << modify_order_ack;
}

void BaseStrategy::on(const roq::CancelOrderAckEvent& event) {
  LOG_IF(FATAL, _download) << "Unexpected";
  const auto& cancel_order_ack = event.cancel_order_ack;
  LOG_IF(INFO, cancel_order_ack.failure == false) <<
      "cancel_order_ack=" << cancel_order_ack;
  LOG_IF(WARNING, cancel_order_ack.failure) <<
      "cancel_order_ack=" << cancel_order_ack;
}

// market data

void BaseStrategy::on(const roq::MarketByPriceEvent& event) {
  const auto& market_by_price = event.market_by_price;
  // return early if it's not the instrument we want to trade
  if (filter(market_by_price.exchange, market_by_price.instrument))
    return;
  // update snapshot view of market data
  std::memcpy(
      _market_data.depth,
      market_by_price.depth,
      sizeof(market_by_price.depth));
  _market_data.exchange_time = market_by_price.exchange_time;
  _market_data.channel = market_by_price.channel;
  _market_data_dirty = true;
}

void BaseStrategy::on(const roq::TradeSummaryEvent& event) {
  const auto& trade_summary = event.trade_summary;
  // return early if it's not the instrument we want to trade
  if (filter(trade_summary.exchange, trade_summary.instrument))
    return;
  // update snapshot view of market data
  _market_data.price = trade_summary.price;
  _market_data.volume = trade_summary.volume;
  _market_data.turnover = trade_summary.turnover;
  _market_data.direction = trade_summary.direction;
  _market_data.exchange_time = trade_summary.exchange_time;
  _market_data.channel = trade_summary.channel;
  _market_data_dirty = true;
}

// Generic function to create an order.
uint32_t BaseStrategy::create_order(
    const roq::TradeDirection direction,
    const double quantity,
    const double price,
    const std::string& order_template) {
  if (is_ready() == false)  // to avoid increasing local order id's for no reason
    throw roq::NotReady();
  auto order_id = ++_max_order_id;
  if (FLAGS_real_trading) {
    roq::CreateOrder create_order {
      .order_id       = order_id,
      .order_template = order_template.c_str(),
      .exchange       = _exchange.c_str(),
      .instrument     = _instrument.c_str(),
      .direction      = direction,
      .quantity       = quantity,
      .limit_price    = price,
    };
    LOG(INFO) << "create_order=" << create_order;
    _dispatcher.send(create_order, _gateway.c_str());
  } else {
    LOG(WARNING) << "Real trading has been disabled: *** ORDER IS NOT CREATED ***";
  }
  return order_id;
}

// general utilities

// Filter update?
// Returns true if the update should be filtered (excluded).
bool BaseStrategy::filter(const char *exchange, const char *instrument) {
  return _instrument.compare(instrument) != 0 || _exchange.compare(exchange) != 0;
}

// Current or start-of-day positions.

double BaseStrategy::get_long_position(PositionType type) const {
  switch (type) {
    case PositionType::StartOfDay: return _long_position_sod;
    case PositionType::NewActivity: return _long_position_new;
    case PositionType::Current: return _long_position_sod + _long_position_new;
  }
}

double BaseStrategy::get_short_position(PositionType type) const {
  switch (type) {
    case PositionType::StartOfDay: return _short_position_sod;
    case PositionType::NewActivity: return _short_position_new;
    case PositionType::Current: return _short_position_sod + _short_position_new;
  }
}

double BaseStrategy::get_net_position(PositionType type) const {
  return get_long_position(type) - get_short_position(type);
}

// Ready to trade?
// Returns true if it may be possible to trade.
// Returns false if it is not possible to trade.
bool BaseStrategy::is_ready() const {
  return !_download && _order_manager_ready && _market_open;
}

// The interface is generic an supposed to work for a multitude of
// gateways. The client therefore has no access to market specific
// fields. Those extra fields are only visible to the gateway. The
// client can, however, leverage such extra fields through templates.
// The gateway will ensure all order updates includes the original
// name of the template. The client can therefore safely compare
// the name of the template with that it used to create the order.
// Returns true if the order template is an "open".
// Returns false if the order template is a "close".
// Terminate program execution if the order template is unknown.
bool BaseStrategy::parse_open_close(const char *order_template) {
  if (FLAGS_ioc_open.compare(order_template) == 0)
    return true;
  if (FLAGS_ioc_close.compare(order_template) == 0)
    return false;
  LOG(FATAL) << "Unknown order_template=\"" << order_template << "\"";
}

}  // namespace simple
}  // namespace examples
