-- SEC Compliance Database Schema
-- Stock Exchange Engine - Production Ready
-- Date: October 21, 2025

-- Orders Table (SEC 17a-3 Compliance - 6 year retention)
CREATE TABLE IF NOT EXISTS orders (
    order_id VARCHAR(64) PRIMARY KEY,
    user_id VARCHAR(64) NOT NULL,
    symbol VARCHAR(10) NOT NULL,
    side INTEGER NOT NULL,        -- 0=BUY, 1=SELL
    type INTEGER NOT NULL,         -- 0=MARKET, 1=LIMIT, 2=IOC, 3=FOK
    quantity BIGINT NOT NULL,
    price DECIMAL(15,4) NOT NULL,  -- Dollars (not cents)
    status VARCHAR(20) NOT NULL,   -- 'open', 'filled', 'partial', 'cancelled'
    timestamp_ms BIGINT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_orders_user_id ON orders(user_id);
CREATE INDEX IF NOT EXISTS idx_orders_symbol ON orders(symbol);
CREATE INDEX IF NOT EXISTS idx_orders_timestamp ON orders(timestamp_ms);
CREATE INDEX IF NOT EXISTS idx_orders_status ON orders(status);

-- Trades Table (SEC 17a-3 Compliance - 6 year retention)
CREATE TABLE IF NOT EXISTS trades (
    trade_id VARCHAR(100) PRIMARY KEY,
    buy_order_id VARCHAR(64) NOT NULL,
    sell_order_id VARCHAR(64) NOT NULL,
    symbol VARCHAR(10) NOT NULL,
    price DECIMAL(15,4) NOT NULL,
    quantity BIGINT NOT NULL,
    buyer_user_id VARCHAR(64) NOT NULL,
    seller_user_id VARCHAR(64) NOT NULL,
    timestamp_ms BIGINT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (buy_order_id) REFERENCES orders(order_id),
    FOREIGN KEY (sell_order_id) REFERENCES orders(order_id)
);

CREATE INDEX IF NOT EXISTS idx_trades_symbol ON trades(symbol);
CREATE INDEX IF NOT EXISTS idx_trades_buyer ON trades(buyer_user_id);
CREATE INDEX IF NOT EXISTS idx_trades_seller ON trades(seller_user_id);
CREATE INDEX IF NOT EXISTS idx_trades_timestamp ON trades(timestamp_ms);

-- Security Events Log (SEC Regulation S-P Compliance)
CREATE TABLE IF NOT EXISTS security_events (
    id SERIAL PRIMARY KEY,
    event_type VARCHAR(50) NOT NULL,  -- 'LOGIN', 'LOGOUT', 'AUTH_FAIL', 'SUSPICIOUS_ORDER', etc.
    user_id VARCHAR(64),
    ip_address INET,
    connection_id VARCHAR(100),
    event_data JSONB,
    severity VARCHAR(20) DEFAULT 'INFO',  -- 'INFO', 'WARNING', 'CRITICAL'
    timestamp_ms BIGINT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_security_events_type ON security_events(event_type);
CREATE INDEX IF NOT EXISTS idx_security_events_user ON security_events(user_id);
CREATE INDEX IF NOT EXISTS idx_security_events_timestamp ON security_events(timestamp_ms);
CREATE INDEX IF NOT EXISTS idx_security_events_severity ON security_events(severity);

-- Circuit Breaker Events (SEC Rule 80B Compliance)
CREATE TABLE IF NOT EXISTS circuit_breaker_events (
    id SERIAL PRIMARY KEY,
    symbol VARCHAR(10) NOT NULL,
    trigger_level INTEGER NOT NULL,  -- 1, 2, or 3 (7%, 13%, 20%)
    trigger_price DECIMAL(15,4) NOT NULL,
    reference_price DECIMAL(15,4) NOT NULL,
    halt_duration_minutes INTEGER NOT NULL,
    halt_start TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    halt_end TIMESTAMP,
    status VARCHAR(20) DEFAULT 'ACTIVE'  -- 'ACTIVE', 'COMPLETED'
);

CREATE INDEX IF NOT EXISTS idx_cb_symbol ON circuit_breaker_events(symbol);
CREATE INDEX IF NOT EXISTS idx_cb_status ON circuit_breaker_events(status);

-- Market Surveillance Alerts (SEC Rule 10b-5 Compliance)
CREATE TABLE IF NOT EXISTS surveillance_alerts (
    id SERIAL PRIMARY KEY,
    alert_type VARCHAR(50) NOT NULL,  -- 'WASH_TRADE', 'SPOOFING', 'LAYERING', etc.
    user_id VARCHAR(64),
    symbol VARCHAR(10),
    related_orders TEXT[],  -- Array of order IDs
    alert_data JSONB,
    severity VARCHAR(20) DEFAULT 'LOW',  -- 'LOW', 'MEDIUM', 'HIGH'
    status VARCHAR(20) DEFAULT 'NEW',  -- 'NEW', 'REVIEWED', 'CLEARED', 'ESCALATED'
    timestamp_ms BIGINT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    reviewed_at TIMESTAMP,
    reviewed_by VARCHAR(64)
);

CREATE INDEX IF NOT EXISTS idx_surveillance_type ON surveillance_alerts(alert_type);
CREATE INDEX IF NOT EXISTS idx_surveillance_user ON surveillance_alerts(user_id);
CREATE INDEX IF NOT EXISTS idx_surveillance_status ON surveillance_alerts(status);
CREATE INDEX IF NOT EXISTS idx_surveillance_severity ON surveillance_alerts(severity);

-- Position Limits Tracking (SEC Rule 15c3-5 Compliance)
CREATE TABLE IF NOT EXISTS position_limits (
    user_id VARCHAR(64) PRIMARY KEY,
    max_position_value BIGINT DEFAULT 1000000000,  -- $10M in cents
    max_single_order_value BIGINT DEFAULT 100000000,  -- $1M in cents
    daily_order_limit INTEGER DEFAULT 10000,
    current_position_value BIGINT DEFAULT 0,
    orders_today INTEGER DEFAULT 0,
    last_reset_date DATE DEFAULT CURRENT_DATE,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Stocks/Companies Master Table
CREATE TABLE IF NOT EXISTS stocks_master (
    symbol VARCHAR(10) PRIMARY KEY,
    company_name VARCHAR(255) NOT NULL,
    sector VARCHAR(100),
    market_cap BIGINT,
    initial_price DECIMAL(15,4) NOT NULL,
    is_active BOOLEAN DEFAULT TRUE,
    listing_date DATE DEFAULT CURRENT_DATE,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Insert default stocks if not exists
INSERT INTO stocks_master (symbol, company_name, sector, initial_price, is_active)
VALUES 
    ('AAPL', 'Apple Inc.', 'Technology', 150.00, TRUE),
    ('MSFT', 'Microsoft Corporation', 'Technology', 330.00, TRUE)
ON CONFLICT (symbol) DO NOTHING;

-- User Accounts Table (enhanced)
CREATE TABLE IF NOT EXISTS user_accounts (
    user_id VARCHAR(64) PRIMARY KEY,
    cash BIGINT NOT NULL DEFAULT 0,  -- In cents
    aapl_qty BIGINT DEFAULT 0,
    googl_qty BIGINT DEFAULT 0,
    msft_qty BIGINT DEFAULT 0,
    amzn_qty BIGINT DEFAULT 0,
    tsla_qty BIGINT DEFAULT 0,
    buying_power BIGINT NOT NULL DEFAULT 0,
    day_trading_buying_power BIGINT NOT NULL DEFAULT 0,
    total_trades BIGINT DEFAULT 0,
    realized_pnl BIGINT DEFAULT 0,
    is_active BOOLEAN DEFAULT TRUE,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Audit Trail View (combines orders and trades for compliance reporting)
CREATE OR REPLACE VIEW audit_trail AS
SELECT 
    o.order_id,
    o.user_id,
    o.symbol,
    o.side,
    o.quantity,
    o.price,
    o.status,
    o.timestamp_ms as order_timestamp,
    t.trade_id,
    t.price as execution_price,
    t.quantity as executed_quantity,
    t.timestamp_ms as execution_timestamp
FROM orders o
LEFT JOIN trades t ON (o.order_id = t.buy_order_id OR o.order_id = t.sell_order_id)
ORDER BY o.timestamp_ms DESC;

-- Function to cleanup old data (for retention policy compliance)
CREATE OR REPLACE FUNCTION cleanup_old_records(retention_years INTEGER DEFAULT 6)
RETURNS INTEGER AS $$
DECLARE
    deleted_count INTEGER := 0;
    cutoff_date TIMESTAMP;
BEGIN
    cutoff_date := NOW() - (retention_years || ' years')::INTERVAL;
    
    -- Delete old orders (keep 6 years for SEC compliance)
    DELETE FROM orders WHERE created_at < cutoff_date;
    GET DIAGNOSTICS deleted_count = ROW_COUNT;
    
    RETURN deleted_count;
END;
$$ LANGUAGE plpgsql;

-- Trigger to update updated_at timestamp
CREATE OR REPLACE FUNCTION update_modified_column()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER update_orders_modtime
    BEFORE UPDATE ON orders
    FOR EACH ROW
    EXECUTE FUNCTION update_modified_column();

CREATE TRIGGER update_stocks_master_modtime
    BEFORE UPDATE ON stocks_master
    FOR EACH ROW
    EXECUTE FUNCTION update_modified_column();

-- Permissions (example - adjust for your security model)
-- GRANT SELECT, INSERT, UPDATE ON ALL TABLES IN SCHEMA public TO trading_engine;
-- GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA public TO trading_engine;
