-- PINN TDSE 1-D benchmark (scripts/gen_pinn_benchmark_sql.py).
-- 32 collocation points × 8 epochs; column ``mdl`` avoids reserved keyword MODEL.

DROP TABLE IF EXISTS bench_log;
DROP TABLE IF EXISTS pinn_model;
DROP TABLE IF EXISTS pts;

CREATE TABLE bench_log (epoch INT, loss TEXT, tag TEXT);
CREATE TABLE pinn_model (id INT, mdl TEXT);
CREATE TABLE pts (id INT, inp TEXT);

INSERT INTO pts (id, inp) VALUES
	(1, 'L[2]:0.10,0.10'),
	(2, 'L[2]:0.15,0.10'),
	(3, 'L[2]:0.20,0.10'),
	(4, 'L[2]:0.25,0.10'),
	(5, 'L[2]:0.30,0.10'),
	(6, 'L[2]:0.35,0.10'),
	(7, 'L[2]:0.40,0.10'),
	(8, 'L[2]:0.45,0.10'),
	(9, 'L[2]:0.10,0.12'),
	(10, 'L[2]:0.15,0.12'),
	(11, 'L[2]:0.20,0.12'),
	(12, 'L[2]:0.25,0.12'),
	(13, 'L[2]:0.30,0.12'),
	(14, 'L[2]:0.35,0.12'),
	(15, 'L[2]:0.40,0.12'),
	(16, 'L[2]:0.45,0.12'),
	(17, 'L[2]:0.10,0.14'),
	(18, 'L[2]:0.15,0.14'),
	(19, 'L[2]:0.20,0.14'),
	(20, 'L[2]:0.25,0.14'),
	(21, 'L[2]:0.30,0.14'),
	(22, 'L[2]:0.35,0.14'),
	(23, 'L[2]:0.40,0.14'),
	(24, 'L[2]:0.45,0.14'),
	(25, 'L[2]:0.10,0.16'),
	(26, 'L[2]:0.15,0.16'),
	(27, 'L[2]:0.20,0.16'),
	(28, 'L[2]:0.25,0.16'),
	(29, 'L[2]:0.30,0.16'),
	(30, 'L[2]:0.35,0.16'),
	(31, 'L[2]:0.40,0.16'),
	(32, 'L[2]:0.45,0.16');

-- Epoch 0
DELETE FROM pinn_model;
INSERT INTO pinn_model (id, mdl) VALUES (
	0,
	MATHSCI_MODEL_BUILD('L[2]:sigmoid,linear', 'LW[2]:T[4,2]:0.5,-0.2, 0.1,0.3, 0.2,0.4, -0.1,0.2;T[2,4]:0.30,0.10,-0.20,0.40, 0.20,-0.10,0.30,0.20')
);
SELECT p.id, PREDICT(m.mdl, p.inp) AS pred FROM pts p, pinn_model m;
SELECT PREDICT(mdl, 'L[2]:0.50,0.20') AS psi_c FROM pinn_model;
SELECT PREDICT(mdl, 'L[2]:0.48,0.20') AS psi_m FROM pinn_model;
SELECT PINN_FD_CENTRAL('0.31', '0.29', '0.02') AS tdse_fd_dx;

-- Epoch 1
DELETE FROM pinn_model;
INSERT INTO pinn_model (id, mdl) VALUES (
	0,
	MATHSCI_MODEL_BUILD('L[2]:sigmoid,linear', 'LW[2]:T[4,2]:0.5,-0.2, 0.1,0.3, 0.2,0.4, -0.1,0.2;T[2,4]:0.28,0.11,-0.18,0.38, 0.19,-0.09,0.31,0.21')
);
SELECT p.id, PREDICT(m.mdl, p.inp) AS pred FROM pts p, pinn_model m;
SELECT PREDICT(mdl, 'L[2]:0.50,0.20') AS psi_c FROM pinn_model;
SELECT PREDICT(mdl, 'L[2]:0.48,0.20') AS psi_m FROM pinn_model;
SELECT PINN_FD_CENTRAL('0.31', '0.29', '0.02') AS tdse_fd_dx;

-- Epoch 2
DELETE FROM pinn_model;
INSERT INTO pinn_model (id, mdl) VALUES (
	0,
	MATHSCI_MODEL_BUILD('L[2]:sigmoid,linear', 'LW[2]:T[4,2]:0.5,-0.2, 0.1,0.3, 0.2,0.4, -0.1,0.2;T[2,4]:0.26,0.12,-0.16,0.36, 0.18,-0.08,0.32,0.22')
);
SELECT p.id, PREDICT(m.mdl, p.inp) AS pred FROM pts p, pinn_model m;
SELECT PREDICT(mdl, 'L[2]:0.50,0.20') AS psi_c FROM pinn_model;
SELECT PREDICT(mdl, 'L[2]:0.48,0.20') AS psi_m FROM pinn_model;
SELECT PINN_FD_CENTRAL('0.31', '0.29', '0.02') AS tdse_fd_dx;

-- Epoch 3
DELETE FROM pinn_model;
INSERT INTO pinn_model (id, mdl) VALUES (
	0,
	MATHSCI_MODEL_BUILD('L[2]:sigmoid,linear', 'LW[2]:T[4,2]:0.5,-0.2, 0.1,0.3, 0.2,0.4, -0.1,0.2;T[2,4]:0.24,0.13,-0.14,0.34, 0.17,-0.07,0.33,0.23')
);
SELECT p.id, PREDICT(m.mdl, p.inp) AS pred FROM pts p, pinn_model m;
SELECT PREDICT(mdl, 'L[2]:0.50,0.20') AS psi_c FROM pinn_model;
SELECT PREDICT(mdl, 'L[2]:0.48,0.20') AS psi_m FROM pinn_model;
SELECT PINN_FD_CENTRAL('0.31', '0.29', '0.02') AS tdse_fd_dx;

-- Epoch 4
DELETE FROM pinn_model;
INSERT INTO pinn_model (id, mdl) VALUES (
	0,
	MATHSCI_MODEL_BUILD('L[2]:sigmoid,linear', 'LW[2]:T[4,2]:0.5,-0.2, 0.1,0.3, 0.2,0.4, -0.1,0.2;T[2,4]:0.22,0.14,-0.12,0.32, 0.16,-0.06,0.34,0.24')
);
SELECT p.id, PREDICT(m.mdl, p.inp) AS pred FROM pts p, pinn_model m;
SELECT PREDICT(mdl, 'L[2]:0.50,0.20') AS psi_c FROM pinn_model;
SELECT PREDICT(mdl, 'L[2]:0.48,0.20') AS psi_m FROM pinn_model;
SELECT PINN_FD_CENTRAL('0.31', '0.29', '0.02') AS tdse_fd_dx;

-- Epoch 5
DELETE FROM pinn_model;
INSERT INTO pinn_model (id, mdl) VALUES (
	0,
	MATHSCI_MODEL_BUILD('L[2]:sigmoid,linear', 'LW[2]:T[4,2]:0.5,-0.2, 0.1,0.3, 0.2,0.4, -0.1,0.2;T[2,4]:0.20,0.15,-0.10,0.30, 0.15,-0.05,0.35,0.25')
);
SELECT p.id, PREDICT(m.mdl, p.inp) AS pred FROM pts p, pinn_model m;
SELECT PREDICT(mdl, 'L[2]:0.50,0.20') AS psi_c FROM pinn_model;
SELECT PREDICT(mdl, 'L[2]:0.48,0.20') AS psi_m FROM pinn_model;
SELECT PINN_FD_CENTRAL('0.31', '0.29', '0.02') AS tdse_fd_dx;

-- Epoch 6
DELETE FROM pinn_model;
INSERT INTO pinn_model (id, mdl) VALUES (
	0,
	MATHSCI_MODEL_BUILD('L[2]:sigmoid,linear', 'LW[2]:T[4,2]:0.5,-0.2, 0.1,0.3, 0.2,0.4, -0.1,0.2;T[2,4]:0.18,0.16,-0.08,0.28, 0.14,-0.04,0.36,0.26')
);
SELECT p.id, PREDICT(m.mdl, p.inp) AS pred FROM pts p, pinn_model m;
SELECT PREDICT(mdl, 'L[2]:0.50,0.20') AS psi_c FROM pinn_model;
SELECT PREDICT(mdl, 'L[2]:0.48,0.20') AS psi_m FROM pinn_model;
SELECT PINN_FD_CENTRAL('0.31', '0.29', '0.02') AS tdse_fd_dx;

-- Epoch 7
DELETE FROM pinn_model;
INSERT INTO pinn_model (id, mdl) VALUES (
	0,
	MATHSCI_MODEL_BUILD('L[2]:sigmoid,linear', 'LW[2]:T[4,2]:0.5,-0.2, 0.1,0.3, 0.2,0.4, -0.1,0.2;T[2,4]:0.16,0.17,-0.06,0.26, 0.13,-0.03,0.37,0.27')
);
SELECT p.id, PREDICT(m.mdl, p.inp) AS pred FROM pts p, pinn_model m;
SELECT PREDICT(mdl, 'L[2]:0.50,0.20') AS psi_c FROM pinn_model;
SELECT PREDICT(mdl, 'L[2]:0.48,0.20') AS psi_m FROM pinn_model;
SELECT PINN_FD_CENTRAL('0.31', '0.29', '0.02') AS tdse_fd_dx;


INSERT INTO bench_log (epoch, loss, tag) VALUES
	(0, '0.000120', 'tdse_residual'),
	(1, '0.000106', 'tdse_residual'),
	(2, '0.000093', 'tdse_residual'),
	(3, '0.000082', 'tdse_residual'),
	(4, '0.000072', 'tdse_residual'),
	(5, '0.000063', 'tdse_residual'),
	(6, '0.000056', 'tdse_residual'),
	(7, '0.000049', 'tdse_residual');

SELECT epoch, loss, tag FROM bench_log ORDER BY epoch, tag;
