-- MySQL Test Case: Comprehensive Basic SQL Operations
-- This script exercises various fundamental SQL features

-- Clean up if tables already exist
DROP TABLE IF EXISTS order_items;
DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS customers;
DROP TABLE IF EXISTS products;

-- Create first table: customers
CREATE TABLE customers (
    customer_id INT PRIMARY KEY AUTO_INCREMENT,
    full_name VARCHAR(100) NOT NULL,
    email VARCHAR(150) UNIQUE,
    registration_date DATE,
    account_balance DECIMAL(10, 2) DEFAULT 0.00,
    is_active BOOLEAN DEFAULT TRUE
);

-- Create second table: products
CREATE TABLE products (
    product_id INT PRIMARY KEY AUTO_INCREMENT,
    product_name VARCHAR(200) NOT NULL,
    category VARCHAR(50),
    price DECIMAL(10, 2) NOT NULL,
    stock_quantity INT DEFAULT 0,
    last_updated TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
);

-- Insert ~20 rows into customers table
INSERT INTO customers (full_name, email, registration_date, account_balance, is_active) VALUES
('John Smith', 'john.smith@email.com', '2024-01-15', 1500.50, TRUE),
('Emma Johnson', 'emma.j@email.com', '2024-02-20', 2750.00, TRUE),
('Michael Brown', 'mbrown@email.com', '2024-01-10', 500.25, TRUE),
('Sarah Davis', 'sarah.davis@email.com', '2024-03-05', 3200.75, FALSE),
('James Wilson', 'jwilson@email.com', '2024-02-28', 150.00, TRUE),
('Lisa Anderson', 'lisa.a@email.com', '2024-01-25', 4500.50, TRUE),
('Robert Taylor', 'rtaylor@email.com', '2024-03-15', 875.25, TRUE),
('Maria Garcia', 'mgarcia@email.com', '2024-02-10', 2100.00, FALSE),
('David Martinez', 'david.m@email.com', '2024-01-30', 650.75, TRUE),
('Jennifer Lee', 'jlee@email.com', '2024-03-20', 3750.50, TRUE),
('William White', 'wwhite@email.com', '2024-02-05', 125.00, TRUE),
('Patricia Harris', 'pharris@email.com', '2024-01-18', 5200.25, TRUE),
('Christopher Clark', 'cclark@email.com', '2024-03-10', 950.50, FALSE),
('Nancy Lewis', 'nlewis@email.com', '2024-02-15', 1875.75, TRUE),
('Daniel Walker', 'dwalker@email.com', '2024-01-22', 425.00, TRUE),
('Karen Hall', 'khall@email.com', '2024-03-25', 2950.50, TRUE),
('Mark Allen', 'mallen@email.com', '2024-02-25', 775.25, FALSE),
('Betty Young', 'byoung@email.com', '2024-01-08', 3400.00, TRUE),
('Kevin King', 'kking@email.com', '2024-03-12', 150.75, TRUE),
('Helen Wright', 'hwright@email.com', '2024-02-18', 4100.50, TRUE);

-- Insert ~20 rows into products table
INSERT INTO products (product_name, category, price, stock_quantity) VALUES
('Laptop Pro 15', 'Electronics', 1299.99, 25),
('Wireless Mouse', 'Electronics', 29.99, 150),
('Office Chair', 'Furniture', 249.99, 40),
('Standing Desk', 'Furniture', 599.99, 15),
('USB-C Cable', 'Electronics', 19.99, 200),
('Monitor 27"', 'Electronics', 399.99, 35),
('Desk Lamp', 'Furniture', 49.99, 80),
('Keyboard Mechanical', 'Electronics', 89.99, 60),
('Bookshelf', 'Furniture', 179.99, 25),
('Webcam HD', 'Electronics', 79.99, 45),
('Desk Organizer', 'Office Supplies', 24.99, 100),
('Printer Ink', 'Office Supplies', 34.99, 200),
('Paper Ream', 'Office Supplies', 8.99, 500),
('Coffee Maker', 'Appliances', 89.99, 30),
('Water Bottle', 'Accessories', 15.99, 150),
('Headphones', 'Electronics', 149.99, 55),
('Notebook Set', 'Office Supplies', 12.99, 250),
('Pen Pack', 'Office Supplies', 9.99, 300),
('Mouse Pad', 'Accessories', 14.99, 175),
('Phone Stand', 'Accessories', 19.99, 90);

-- Create indexes on the products table
CREATE INDEX idx_category ON products(category);
CREATE INDEX idx_price ON products(price);

-- Update some rows
UPDATE customers 
SET account_balance = account_balance + 100.00 
WHERE is_active = TRUE AND account_balance < 1000;

UPDATE products 
SET stock_quantity = stock_quantity - 5 
WHERE category = 'Electronics' AND stock_quantity > 50;

-- Delete some rows
DELETE FROM customers 
WHERE is_active = FALSE AND account_balance < 1000;

DELETE FROM products 
WHERE stock_quantity = 0;

-- Select operations with various conditions
SELECT * FROM customers 
WHERE account_balance > 2000 
ORDER BY registration_date DESC 
LIMIT 5;

SELECT product_name, category, price 
FROM products 
WHERE price BETWEEN 20 AND 100 
ORDER BY price ASC;

-- Aggregation queries
SELECT category, 
       COUNT(*) as product_count, 
       AVG(price) as avg_price,
       MAX(price) as max_price,
       MIN(price) as min_price
FROM products 
GROUP BY category
HAVING COUNT(*) > 2;

-- Join operations
SELECT c.full_name, c.email, c.account_balance,
       COUNT(DISTINCT p.category) as categories_viewed
FROM customers c
CROSS JOIN products p
WHERE c.is_active = TRUE 
  AND p.price <= c.account_balance
GROUP BY c.customer_id, c.full_name, c.email, c.account_balance
HAVING categories_viewed > 0
ORDER BY c.account_balance DESC;

-- Create a temporary orders table for more complex joins
CREATE TABLE orders (
    order_id INT PRIMARY KEY AUTO_INCREMENT,
    customer_id INT,
    order_date DATE,
    total_amount DECIMAL(10, 2),
    FOREIGN KEY (customer_id) REFERENCES customers(customer_id)
);

INSERT INTO orders (customer_id, order_date, total_amount) VALUES
(1, '2024-04-01', 299.99),
(2, '2024-04-02', 149.99),
(1, '2024-04-03', 89.99),
(3, '2024-04-04', 599.99),
(5, '2024-04-05', 49.99);

-- Inner join
SELECT c.full_name, o.order_date, o.total_amount
FROM customers c
INNER JOIN orders o ON c.customer_id = o.customer_id
WHERE o.total_amount > 100
ORDER BY o.order_date;

-- Left join
SELECT c.full_name, c.email, 
       COALESCE(SUM(o.total_amount), 0) as total_orders
FROM customers c
LEFT JOIN orders o ON c.customer_id = o.customer_id
GROUP BY c.customer_id, c.full_name, c.email
ORDER BY total_orders DESC;

-- Subquery example
SELECT * FROM products
WHERE price > (SELECT AVG(price) FROM products)
ORDER BY price DESC;

-- Union example
SELECT 'High Value Customer' as type, full_name, account_balance
FROM customers
WHERE account_balance > 3000
UNION
SELECT 'Recent Customer' as type, full_name, account_balance
FROM customers
WHERE registration_date >= '2024-03-01'
ORDER BY account_balance DESC;

-- Drop the products table
DROP TABLE products;

-- Recreate products table with same schema
CREATE TABLE products (
    product_id INT PRIMARY KEY AUTO_INCREMENT,
    product_name VARCHAR(200) NOT NULL,
    category VARCHAR(50),
    price DECIMAL(10, 2) NOT NULL,
    stock_quantity INT DEFAULT 0,
    last_updated TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
);

-- Re-insert test data into the new products table
INSERT INTO products (product_name, category, price, stock_quantity) VALUES
('Smartphone X', 'Electronics', 899.99, 20),
('Tablet Pro', 'Electronics', 599.99, 30),
('Smart Watch', 'Electronics', 299.99, 50),
('Bluetooth Speaker', 'Electronics', 79.99, 75),
('Power Bank', 'Electronics', 39.99, 100),
('Cable Set', 'Accessories', 24.99, 150),
('Screen Protector', 'Accessories', 14.99, 200),
('Phone Case', 'Accessories', 19.99, 180),
('Car Charger', 'Accessories', 29.99, 90),
('Wireless Earbuds', 'Electronics', 149.99, 40),
('Laptop Bag', 'Accessories', 49.99, 60),
('External SSD', 'Electronics', 129.99, 35),
('HDMI Cable', 'Accessories', 12.99, 250),
('USB Hub', 'Electronics', 34.99, 80),
('Cooling Pad', 'Accessories', 39.99, 45),
('Memory Card', 'Electronics', 29.99, 120),
('Router WiFi 6', 'Electronics', 199.99, 25),
('Ethernet Cable', 'Accessories', 9.99, 300),
('Surge Protector', 'Electronics', 44.99, 70),
('Cable Organizer', 'Accessories', 11.99, 200);

-- Recreate indexes
CREATE INDEX idx_category_new ON products(category);
CREATE INDEX idx_price_new ON products(price);

-- Test the recreated table with similar operations
SELECT category, 
       COUNT(*) as item_count,
       ROUND(AVG(price), 2) as average_price
FROM products
GROUP BY category
ORDER BY item_count DESC;

-- Complex filtering with multiple conditions
SELECT product_name, category, price, stock_quantity,
       CASE 
           WHEN price > 500 THEN 'Premium'
           WHEN price > 100 THEN 'Standard'
           ELSE 'Budget'
       END as price_tier,
       CASE
           WHEN stock_quantity > 100 THEN 'High Stock'
           WHEN stock_quantity > 50 THEN 'Medium Stock'
           ELSE 'Low Stock'
       END as stock_level
FROM products
WHERE (category = 'Electronics' AND price > 50) 
   OR (category = 'Accessories' AND stock_quantity > 100)
ORDER BY price DESC, stock_quantity DESC;

-- Final verification query
SELECT 'Customers' as table_name, COUNT(*) as row_count FROM customers
UNION ALL
SELECT 'Products' as table_name, COUNT(*) as row_count FROM products
UNION ALL
SELECT 'Orders' as table_name, COUNT(*) as row_count FROM orders;

-- =====================================
-- TRANSACTION EXAMPLES
-- =====================================

-- Transaction 1: Successful multi-table update with COMMIT
START TRANSACTION;

-- Update customer balance
UPDATE customers 
SET account_balance = account_balance - 299.99
WHERE customer_id = 1;

-- Insert a new order
INSERT INTO orders (customer_id, order_date, total_amount) 
VALUES (1, '2024-04-10', 299.99);

-- Update product stock
UPDATE products 
SET stock_quantity = stock_quantity - 1
WHERE product_name = 'Smart Watch';

-- Verify the changes before committing
SELECT 'Transaction 1 - Before Commit' as status, 
       account_balance 
FROM customers 
WHERE customer_id = 1;

COMMIT;

-- Transaction 2: Rollback example for insufficient stock
START TRANSACTION;

-- Set a savepoint
SAVEPOINT before_order;

-- Try to place a large order
UPDATE products 
SET stock_quantity = stock_quantity - 1000
WHERE product_name = 'Tablet Pro';

-- Check if stock went negative (this would violate business logic)
SELECT product_name, stock_quantity 
FROM products 
WHERE product_name = 'Tablet Pro';

-- Rollback if stock would be negative
ROLLBACK TO SAVEPOINT before_order;

-- Try a smaller, valid order instead
UPDATE products 
SET stock_quantity = stock_quantity - 2
WHERE product_name = 'Tablet Pro';

INSERT INTO orders (customer_id, order_date, total_amount)
VALUES (2, '2024-04-11', 1199.98);

COMMIT;

-- Transaction 3: Transfer funds between customers with validation
START TRANSACTION;

-- Set variables for the transfer
SET @transfer_amount = 500.00;
SET @from_customer = 2;
SET @to_customer = 3;

-- Check sender's balance
SELECT @sender_balance := account_balance 
FROM customers 
WHERE customer_id = @from_customer;

-- Only proceed if sufficient funds
UPDATE customers 
SET account_balance = account_balance - @transfer_amount
WHERE customer_id = @from_customer 
  AND account_balance >= @transfer_amount;

UPDATE customers 
SET account_balance = account_balance + @transfer_amount
WHERE customer_id = @to_customer
  AND EXISTS (
    SELECT 1 FROM (
      SELECT customer_id FROM customers 
      WHERE customer_id = @from_customer 
        AND account_balance >= @transfer_amount
    ) as temp
  );

-- Verify both balances
SELECT customer_id, full_name, account_balance 
FROM customers 
WHERE customer_id IN (@from_customer, @to_customer);

COMMIT;

-- Transaction 4: Batch product price update with audit trail
START TRANSACTION;

-- Create temporary audit table for this transaction
CREATE TEMPORARY TABLE IF NOT EXISTS price_audit (
    product_id INT,
    old_price DECIMAL(10, 2),
    new_price DECIMAL(10, 2),
    change_date TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Record current prices before update
INSERT INTO price_audit (product_id, old_price, new_price)
SELECT product_id, price, price * 1.10
FROM products
WHERE category = 'Electronics';

-- Apply 10% price increase to all electronics
UPDATE products 
SET price = price * 1.10
WHERE category = 'Electronics';

-- Verify the changes
SELECT p.product_name, pa.old_price, pa.new_price,
       ROUND((pa.new_price - pa.old_price), 2) as increase
FROM products p
JOIN price_audit pa ON p.product_id = pa.product_id
ORDER BY increase DESC;

COMMIT;

-- Transaction 5: Complex order processing with inventory check
DELIMITER //
START TRANSACTION//

-- Create order with multiple items
SET @new_order_id = NULL//

INSERT INTO orders (customer_id, order_date, total_amount)
VALUES (5, '2024-04-12', 0)//

SET @new_order_id = LAST_INSERT_ID()//

-- Create order_items table for this example
CREATE TABLE IF NOT EXISTS order_items (
    item_id INT PRIMARY KEY AUTO_INCREMENT,
    order_id INT,
    product_id INT,
    quantity INT,
    unit_price DECIMAL(10, 2),
    FOREIGN KEY (order_id) REFERENCES orders(order_id),
    FOREIGN KEY (product_id) REFERENCES products(product_id)
)//

-- Add multiple items to the order
INSERT INTO order_items (order_id, product_id, quantity, unit_price)
SELECT @new_order_id, product_id, 2, price
FROM products
WHERE product_name IN ('Bluetooth Speaker', 'Power Bank', 'Cable Set')//

-- Update product inventory
UPDATE products p
JOIN order_items oi ON p.product_id = oi.product_id
SET p.stock_quantity = p.stock_quantity - oi.quantity
WHERE oi.order_id = @new_order_id//

-- Calculate and update order total
UPDATE orders o
SET total_amount = (
    SELECT SUM(quantity * unit_price)
    FROM order_items
    WHERE order_id = @new_order_id
)
WHERE order_id = @new_order_id//

-- Update customer balance
UPDATE customers c
JOIN orders o ON c.customer_id = o.customer_id
SET c.account_balance = c.account_balance - o.total_amount
WHERE o.order_id = @new_order_id//

COMMIT//
DELIMITER ;

-- Transaction 6: Deadlock prevention example with proper locking order
START TRANSACTION;

-- Lock resources in consistent order to prevent deadlock
SELECT * FROM customers WHERE customer_id = 1 FOR UPDATE;
SELECT * FROM products WHERE product_id = 1 FOR UPDATE;

-- Perform updates
UPDATE customers SET account_balance = account_balance - 100 WHERE customer_id = 1;
UPDATE products SET stock_quantity = stock_quantity - 1 WHERE product_id = 1;

COMMIT;

-- Transaction 7: Using READ COMMITTED isolation level
SET TRANSACTION ISOLATION LEVEL READ COMMITTED;
START TRANSACTION;

-- Read data that might be modified by concurrent transactions
SELECT SUM(account_balance) as total_balance FROM customers;
SELECT SUM(stock_quantity * price) as inventory_value FROM products;

-- Perform calculations based on current snapshot
INSERT INTO orders (customer_id, order_date, total_amount)
SELECT 
    customer_id,
    CURRENT_DATE,
    account_balance * 0.01 as loyalty_bonus
FROM customers
WHERE account_balance > 2000
LIMIT 3;

COMMIT;

-- Reset to default isolation level
SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;

-- Final verification of all tables after transactions
SELECT 'Final Table Counts After Transactions:' as summary;
SELECT 'Customers' as table_name, COUNT(*) as row_count FROM customers
UNION ALL
SELECT 'Products' as table_name, COUNT(*) as row_count FROM products
UNION ALL
SELECT 'Orders' as table_name, COUNT(*) as row_count FROM orders
UNION ALL
SELECT 'Order Items' as table_name, COUNT(*) as row_count FROM order_items;

-- Clean up (optional - uncomment if you want to drop all tables at the end)
DROP TABLE IF EXISTS order_items;
DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS customers;
DROP TABLE IF EXISTS products;
