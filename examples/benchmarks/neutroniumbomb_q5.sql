-- Query 5: PINN training
SELECT
    epoch,
    loss,
    PREDICT(model, x) AS prediction
FROM (
    WITH RECURSIVE pinn_train(epoch, model, loss) AS (
        SELECT
            0,
            MATHSCI_MODEL_BUILD('L[2]:sigmoid,linear', weights),
            1.0
        FROM initial_weights
        UNION ALL
        SELECT
            epoch + 1,
            OPTIMIZER_STEP(model, gradient),
            MSE_LOSS(PREDICT(model, x), target)
        FROM pinn_train, training_data
        WHERE epoch < 100
    )
    SELECT * FROM pinn_train
) WHERE epoch % 10 = 0
ORDER BY epoch;

