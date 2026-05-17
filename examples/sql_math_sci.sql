-- Elementary math, statistics/ML/scientific builtins, and LIST type.

CREATE TABLE lab (
	id INT,
	vals LIST(DOUBLE),
	scores LIST(INTEGER)
);

INSERT INTO lab VALUES (
	1,
	'L[4]:1,2,3,4',
	'L[3]:10,20,30'
);

SELECT
	id,
	ABS(-3.5) AS a,
	SQRT(16) AS r,
	POW(2, 10) AS p,
	MEAN(vals) AS mu,
	STDDEV_SAMP(vals) AS sigma,
	LIST_LEN(scores) AS n,
	LIST_GET(scores, 1) AS second,
	SIGMOID(0) AS s0,
	NORM_L2(vals) AS l2
FROM lab;

SELECT LIST_CONCAT(vals, 'L[1]:5') AS extended FROM lab;

SELECT
	LOGISTIC(0) AS lg,
	MSE_LOSS('L[2]:0,0', 'L[2]:1,1') AS mse,
	COSINE_SIM(vals, 'L[4]:1,2,3,4') AS sim,
	LIST_SORT_DESC(vals) AS desc_sorted,
	SETSEED(7) AS seed,
	RANDOM() AS u
FROM lab;

-- Signal processing (SIMD) and autograd primitives (see docs/MathSciSignal.md).
SELECT
	FFT('L[4]:1,0,1,0') AS spectrum,
	CONV_FULL('L[3]:1,2,3', 'L[2]:1,1') AS conv_full,
	LAPLACIAN('L[4]:0,1,2,3') AS lap,
	AD_CHAIN('L[2]:0.5,0.5', 'L[2]:2,4') AS chain_grad
FROM lab;

-- Hessian, Wirtinger AD, and ODE/SDE/PDE steppers (see docs/MathSciAutograd.md, docs/MathSciSolves.md).
SELECT
	AD_HESSIAN_SIGMOID('L[2]:0.5,0.6', 'L[2]:1,1') AS hess_sig,
	AD_WIRTINGER_DZ('L[4]:1,0,1,0') AS wdz,
	ODE_EULER('L[2]:1,0', 'L[2]:0.1,0.2', '0.01') AS euler_step,
	SDE_GBM(100, 0.05, 0.2, 0.01, 0) AS gbm_step
FROM lab;
