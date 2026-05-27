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
	ODE_HEUN('L[2]:1,0', 'L[2]:0.1,0', 'L[2]:0.12,0', '0.01') AS heun_step,
	ODE_IMPLICIT_EULER('L[2]:1,0', 'L[2]:0.5,0.5', '0.01') AS implicit_step,
	SOLVE_ODE('RK4', 'L[2]:1,0', 'L[2]:0.1,0', 'L[2]:0.11,0', 'L[2]:0.12,0', 'L[2]:0.13,0', '0.01') AS rk4_step,
	SDE_GBM(100, 0.05, 0.2, 0.01, 0) AS gbm_step,
	SDE_MILSTEIN(100, 0.05, 0.2, 0.01, 0) AS milstein_step,
	PDE_ADVECTION_STEP('L[4]:0,1,2,3', 0.5, 0.01, 1) AS adv_step,
	ODE_TRAPEZOID('L[2]:1,0', 'L[2]:0.1,0', 'L[2]:0.12,0', '0.01') AS trap_step,
	ODE_SEMI_IMPLICIT('L[2]:1,0', 'L[2]:0.1,0', 'L[2]:0.05,0', '0.01') AS semi_step,
	SOLVE_ODE('TRAPEZOID', 'L[2]:1,0', 'L[2]:0.1,0', 'L[2]:0.12,0', 'L[1]:0', 'L[1]:0', '0.01') AS trap_solve,
	ODE_RK3('L[1]:1', 'L[1]:0.1', 'L[1]:0.12', 'L[1]:0.13', '0.1') AS rk3_step,
	LINEAR_CG_SOLVE('T[2,2]:4,1,1,3', 'L[2]:0,0', 'L[2]:1,2', 32, 1e-6) AS cg_x,
	ROOT_NEWTON_STEP(2, 1, 2) AS newton_x,
	ODE_MARCH('EULER', 'L[1]:1', 'L[1]:0.5', 'L[1]:0', 'L[1]:0', 'L[1]:0', '0.1', 2) AS euler_march
FROM lab;

-- Classifiers, NLP, and embeddings (see docs/MathSciClassify.md, MathSciNlp.md, MathSciEmbeddings.md).
SELECT
	NLP_TOKENIZE('hello world') AS toks,
	NLP_NGRAMS('hello world', 2) AS bigrams,
	NLP_JACCARD('hello world', 'hello there') AS jac,
	NLP_STEM('running') AS stem,
	CLASSIFY_ARGMAX('L[3]:0.1,0.5,0.2') AS best,
	NLP_EMBED_BUILD('L[2]:a,b', 'T[2,2]:1,0,0,1') AS emb,
	NLP_EMBED_LOOKUP(
		NLP_EMBED_BUILD('L[2]:a,b', 'T[2,2]:1,0,0,1'),
		'a'
	) AS vec_a,
	NLP_EMBED_BUILD('L[2]:x,y', 'TC[2,2]:1,0,0,1,0,1,1,0') AS cemb,
	COSINE_SIM('CV[2]:1,0,0,1', 'CV[2]:0,1,1,0') AS c_ortho
FROM lab;

-- Catalog-backed complex embeddings (see docs/MathSciEmbeddings.md).
CREATE TABLE emb_src (tok TEXT, vec TEXT);
INSERT INTO emb_src VALUES
	('alpha', 'CV[2]:1,0,0,1'),
	('beta',  'CV[2]:0,1,1,0');
CREATE EMBEDDING wordvec AS TABLE emb_src (tok, vec);

SELECT
	NLP_EMBED_LOOKUP('@wordvec', 'alpha') AS alpha_vec,
	NLP_EMBED_MEAN('L[2]:alpha,beta', '@wordvec') AS mean_vec
FROM lab;
