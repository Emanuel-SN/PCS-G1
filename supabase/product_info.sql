INSERT INTO product_info (product_name, T_min, T_max, H_min, H_max, base_price, unit)
VALUES ('Banana', 13, 25, 50, 80, 4.00, 'kg')
RETURNING product_id;