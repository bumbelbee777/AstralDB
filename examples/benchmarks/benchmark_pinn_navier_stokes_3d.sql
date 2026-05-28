-- PINN 3-D Navier-Stokes benchmark (scripts/gen_pinn_benchmark_sql.py).

DROP TABLE IF EXISTS bench_log;
DROP TABLE IF EXISTS pinn_model;
DROP TABLE IF EXISTS pts;

CREATE TABLE bench_log (epoch INT, loss TEXT, tag TEXT);
CREATE TABLE pinn_model (id INT, mdl TEXT);
CREATE TABLE pts (id INT, inp TEXT);

INSERT INTO pts (id, inp) VALUES
	(1, 'L[3]:0.10,0.20,0.30'),
	(2, 'L[3]:0.16,0.20,0.30'),
	(3, 'L[3]:0.22,0.20,0.30'),
	(4, 'L[3]:0.28,0.20,0.30'),
	(5, 'L[3]:0.34,0.20,0.30'),
	(6, 'L[3]:0.40,0.20,0.30'),
	(7, 'L[3]:0.10,0.25,0.30'),
	(8, 'L[3]:0.16,0.25,0.30'),
	(9, 'L[3]:0.22,0.25,0.30'),
	(10, 'L[3]:0.28,0.25,0.30'),
	(11, 'L[3]:0.34,0.25,0.30'),
	(12, 'L[3]:0.40,0.25,0.30'),
	(13, 'L[3]:0.10,0.30,0.30'),
	(14, 'L[3]:0.16,0.30,0.30'),
	(15, 'L[3]:0.22,0.30,0.30'),
	(16, 'L[3]:0.28,0.30,0.30'),
	(17, 'L[3]:0.34,0.30,0.30'),
	(18, 'L[3]:0.40,0.30,0.30'),
	(19, 'L[3]:0.10,0.35,0.30'),
	(20, 'L[3]:0.16,0.35,0.30'),
	(21, 'L[3]:0.22,0.35,0.30'),
	(22, 'L[3]:0.28,0.35,0.30'),
	(23, 'L[3]:0.34,0.35,0.30'),
	(24, 'L[3]:0.40,0.35,0.30');

DELETE FROM pinn_model;
INSERT INTO pinn_model (id, mdl) VALUES (
	0,
	MATHSCI_MODEL_BUILD('L[2]:sigmoid,linear', 'LW[2]:T[6,3]:0.1,0.2,0, 0.1,0,0.2, 0,0.1,0.2, 0.1,0,0, 0.1,0,0.1, 0.2,0,0.1;T[4,6]:0.20,0,0,0.1,0,0, 0,0.2,0,0.1,0,0, 0,0,0.2,0.1,0,0, 0.1,0.1,0.1,0.1,0,0')
);
SELECT p.id, PREDICT(m.mdl, p.inp) AS pred FROM pts p, pinn_model m;
SELECT PREDICT(mdl, 'L[3]:0.22,0.30,0.40') AS u_xp FROM pinn_model;
SELECT PREDICT(mdl, 'L[3]:0.18,0.30,0.40') AS u_xm FROM pinn_model;
SELECT PINN_FD_CENTRAL('0.53', '0.51', '0.02') AS ns_fd_dx;

DELETE FROM pinn_model;
INSERT INTO pinn_model (id, mdl) VALUES (
	0,
	MATHSCI_MODEL_BUILD('L[2]:sigmoid,linear', 'LW[2]:T[6,3]:0.1,0.2,0, 0.1,0,0.2, 0,0.1,0.2, 0.1,0,0, 0.1,0,0.1, 0.2,0,0.1;T[4,6]:0.19,0.01,0,0.1,0,0, 0.01,0.19,0,0.1,0,0, 0,0,0.19,0.1,0,0, 0.09,0.09,0.09,0.11,0,0')
);
SELECT p.id, PREDICT(m.mdl, p.inp) AS pred FROM pts p, pinn_model m;
SELECT PREDICT(mdl, 'L[3]:0.22,0.30,0.40') AS u_xp FROM pinn_model;
SELECT PREDICT(mdl, 'L[3]:0.18,0.30,0.40') AS u_xm FROM pinn_model;
SELECT PINN_FD_CENTRAL('0.53', '0.51', '0.02') AS ns_fd_dx;

DELETE FROM pinn_model;
INSERT INTO pinn_model (id, mdl) VALUES (
	0,
	MATHSCI_MODEL_BUILD('L[2]:sigmoid,linear', 'LW[2]:T[6,3]:0.1,0.2,0, 0.1,0,0.2, 0,0.1,0.2, 0.1,0,0, 0.1,0,0.1, 0.2,0,0.1;T[4,6]:0.18,0.02,0,0.1,0,0, 0.02,0.18,0,0.1,0,0, 0,0,0.18,0.1,0,0, 0.08,0.08,0.08,0.12,0,0')
);
SELECT p.id, PREDICT(m.mdl, p.inp) AS pred FROM pts p, pinn_model m;
SELECT PREDICT(mdl, 'L[3]:0.22,0.30,0.40') AS u_xp FROM pinn_model;
SELECT PREDICT(mdl, 'L[3]:0.18,0.30,0.40') AS u_xm FROM pinn_model;
SELECT PINN_FD_CENTRAL('0.53', '0.51', '0.02') AS ns_fd_dx;

DELETE FROM pinn_model;
INSERT INTO pinn_model (id, mdl) VALUES (
	0,
	MATHSCI_MODEL_BUILD('L[2]:sigmoid,linear', 'LW[2]:T[6,3]:0.1,0.2,0, 0.1,0,0.2, 0,0.1,0.2, 0.1,0,0, 0.1,0,0.1, 0.2,0,0.1;T[4,6]:0.17,0.03,0,0.1,0,0, 0.03,0.17,0,0.1,0,0, 0,0,0.17,0.1,0,0, 0.07,0.07,0.07,0.13,0,0')
);
SELECT p.id, PREDICT(m.mdl, p.inp) AS pred FROM pts p, pinn_model m;
SELECT PREDICT(mdl, 'L[3]:0.22,0.30,0.40') AS u_xp FROM pinn_model;
SELECT PREDICT(mdl, 'L[3]:0.18,0.30,0.40') AS u_xm FROM pinn_model;
SELECT PINN_FD_CENTRAL('0.53', '0.51', '0.02') AS ns_fd_dx;

DELETE FROM pinn_model;
INSERT INTO pinn_model (id, mdl) VALUES (
	0,
	MATHSCI_MODEL_BUILD('L[2]:sigmoid,linear', 'LW[2]:T[6,3]:0.1,0.2,0, 0.1,0,0.2, 0,0.1,0.2, 0.1,0,0, 0.1,0,0.1, 0.2,0,0.1;T[4,6]:0.16,0.04,0,0.1,0,0, 0.04,0.16,0,0.1,0,0, 0,0,0.16,0.1,0,0, 0.06,0.06,0.06,0.14,0,0')
);
SELECT p.id, PREDICT(m.mdl, p.inp) AS pred FROM pts p, pinn_model m;
SELECT PREDICT(mdl, 'L[3]:0.22,0.30,0.40') AS u_xp FROM pinn_model;
SELECT PREDICT(mdl, 'L[3]:0.18,0.30,0.40') AS u_xm FROM pinn_model;
SELECT PINN_FD_CENTRAL('0.53', '0.51', '0.02') AS ns_fd_dx;

DELETE FROM pinn_model;
INSERT INTO pinn_model (id, mdl) VALUES (
	0,
	MATHSCI_MODEL_BUILD('L[2]:sigmoid,linear', 'LW[2]:T[6,3]:0.1,0.2,0, 0.1,0,0.2, 0,0.1,0.2, 0.1,0,0, 0.1,0,0.1, 0.2,0,0.1;T[4,6]:0.15,0.05,0,0.1,0,0, 0.05,0.15,0,0.1,0,0, 0,0,0.15,0.1,0,0, 0.05,0.05,0.05,0.15,0,0')
);
SELECT p.id, PREDICT(m.mdl, p.inp) AS pred FROM pts p, pinn_model m;
SELECT PREDICT(mdl, 'L[3]:0.22,0.30,0.40') AS u_xp FROM pinn_model;
SELECT PREDICT(mdl, 'L[3]:0.18,0.30,0.40') AS u_xm FROM pinn_model;
SELECT PINN_FD_CENTRAL('0.53', '0.51', '0.02') AS ns_fd_dx;

DELETE FROM pinn_model;
INSERT INTO pinn_model (id, mdl) VALUES (
	0,
	MATHSCI_MODEL_BUILD('L[2]:sigmoid,linear', 'LW[2]:T[6,3]:0.1,0.2,0, 0.1,0,0.2, 0,0.1,0.2, 0.1,0,0, 0.1,0,0.1, 0.2,0,0.1;T[4,6]:0.14,0.06,0,0.1,0,0, 0.06,0.14,0,0.1,0,0, 0,0,0.14,0.1,0,0, 0.04,0.04,0.04,0.16,0,0')
);
SELECT p.id, PREDICT(m.mdl, p.inp) AS pred FROM pts p, pinn_model m;
SELECT PREDICT(mdl, 'L[3]:0.22,0.30,0.40') AS u_xp FROM pinn_model;
SELECT PREDICT(mdl, 'L[3]:0.18,0.30,0.40') AS u_xm FROM pinn_model;
SELECT PINN_FD_CENTRAL('0.53', '0.51', '0.02') AS ns_fd_dx;

DELETE FROM pinn_model;
INSERT INTO pinn_model (id, mdl) VALUES (
	0,
	MATHSCI_MODEL_BUILD('L[2]:sigmoid,linear', 'LW[2]:T[6,3]:0.1,0.2,0, 0.1,0,0.2, 0,0.1,0.2, 0.1,0,0, 0.1,0,0.1, 0.2,0,0.1;T[4,6]:0.13,0.07,0,0.1,0,0, 0.07,0.13,0,0.1,0,0, 0,0,0.13,0.1,0,0, 0.03,0.03,0.03,0.17,0,0')
);
SELECT p.id, PREDICT(m.mdl, p.inp) AS pred FROM pts p, pinn_model m;
SELECT PREDICT(mdl, 'L[3]:0.22,0.30,0.40') AS u_xp FROM pinn_model;
SELECT PREDICT(mdl, 'L[3]:0.18,0.30,0.40') AS u_xm FROM pinn_model;
SELECT PINN_FD_CENTRAL('0.53', '0.51', '0.02') AS ns_fd_dx;


INSERT INTO bench_log (epoch, loss, tag) VALUES
	(0, '0.000420', 'ns_div'),
	(1, '0.000378', 'ns_div'),
	(2, '0.000340', 'ns_div'),
	(3, '0.000306', 'ns_div'),
	(4, '0.000276', 'ns_div'),
	(5, '0.000248', 'ns_div'),
	(6, '0.000223', 'ns_div'),
	(7, '0.000201', 'ns_div');

SELECT epoch, loss, tag FROM bench_log ORDER BY epoch, tag;
