-- Steady 3-D incompressible Navier-Stokes PINN toy (docs/MathSciMl.md).
-- Uses PREDICT(), binary model I/O, and PINN finite-difference helpers.

DROP TABLE IF EXISTS pinn_ns_model;
DROP TABLE IF EXISTS pinn_ns_w;
DROP TABLE IF EXISTS pinn_ns_pts;

CREATE TABLE pinn_ns_pts (
	id INT,
	x TEXT,
	y TEXT,
	z TEXT,
	tag TEXT
);
INSERT INTO pinn_ns_pts VALUES
	(1, '0.2', '0.3', '0.4', 'center'),
	(2, '0.22', '0.3', '0.4', 'x_plus'),
	(3, '0.2', '0.32', '0.4', 'y_plus'),
	(4, '0.2', '0.3', '0.42', 'z_plus');

CREATE TABLE pinn_ns_w (id INT, w1 TEXT, w2 TEXT);
INSERT INTO pinn_ns_w VALUES (
	0,
	'T[6,3]:0.1,0.2,0, 0.1,0,0.2, 0,0.1,0.2, 0.1,0,0, 0.1,0,0.1, 0.2,0,0.1',
	'T[4,6]:0.2,0,0,0.1,0,0, 0,0.2,0,0.1,0,0, 0,0,0.2,0.1,0,0, 0.1,0.1,0.1,0.1,0,0'
);

CREATE TABLE pinn_ns_model (id INT, mdl TEXT);
INSERT INTO pinn_ns_model VALUES (
	0,
	MATHSCI_MODEL_BUILD(
		'L[2]:sigmoid,linear',
		'LW[2]:T[6,3]:0.1,0.2,0, 0.1,0,0.2, 0,0.1,0.2, 0.1,0,0, 0.1,0,0.1, 0.2,0,0.1;T[4,6]:0.2,0,0,0.1,0,0, 0,0.2,0,0.1,0,0, 0,0,0.2,0.1,0,0, 0.1,0.1,0.1,0.1,0,0'
	)
);

SELECT PREDICT(mdl, 'L[3]:0.2,0.3,0.4') AS uvwp_predict FROM pinn_ns_model WHERE id = 0;

SELECT MATVEC(w1, 'L[3]:0.2,0.3,0.4') AS lin_center FROM pinn_ns_w WHERE id = 0;
SELECT SIGMOID('L[6]:0.52,0.55,0.50,0.53,0.51,0.54') AS h_center;
SELECT MATVEC(w2, 'L[6]:0.52,0.55,0.50,0.53,0.51,0.54') AS uvwp_center FROM pinn_ns_w WHERE id = 0;

SELECT
	MSE_LOSS('L[3]:0.02,0.01,-0.01', 'L[3]:0,0,0') AS div_u_proxy,
	PINN_FD_CENTRAL('0.53', '0.51', '0.02') AS du_dx_fd,
	MSE_LOSS('L[4]:0.05,0.04,0.03,0.10', 'L[4]:0,0,0,0') AS momentum_residual_proxy;

SELECT AD_GRAD_MATVEC_IN(w2, 'L[4]:0.01,0.01,0.01,0.01') AS grad_h FROM pinn_ns_w WHERE id = 0;

INSERT INTO pinn_ns_w VALUES (
	1,
	'T[6,3]:0.1,0.2,0, 0.1,0,0.2, 0,0.1,0.2, 0.1,0,0, 0.1,0,0.1, 0.2,0,0.1',
	'T[4,6]:0.19,0.01,0,0.1,0,0, 0.01,0.19,0,0.1,0,0, 0,0,0.19,0.1,0,0, 0.09,0.09,0.09,0.11,0,0'
);

INSERT INTO pinn_ns_model SELECT
	1,
	MATHSCI_MODEL_LOAD(MATHSCI_MODEL_SERIALIZE(mdl))
FROM pinn_ns_model WHERE id = 0;

SELECT id, w2 FROM pinn_ns_w ORDER BY id;

DROP TABLE IF EXISTS pinn_ns_model;
DROP TABLE IF EXISTS pinn_ns_w;
DROP TABLE IF EXISTS pinn_ns_pts;
