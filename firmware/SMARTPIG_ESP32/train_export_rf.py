"""SMARTPIG - train the Random Forest Regressor and export it to an Arduino header.

Run:   pip install pandas scikit-learn micromlgen
       python train_export_rf.py
Output: RandomForestRegressor.h  (copy next to SMARTPIG_ESP32.ino)

The 11 features MUST stay in this exact order (same as x[0..10] in the sketch).
"""
import pandas as pd
from micromlgen import port
from sklearn.ensemble import RandomForestRegressor
from sklearn.metrics import mean_absolute_error
from sklearn.model_selection import GroupKFold, cross_val_predict

FEATURES = ["Age_days", "Stage_code", "Meal_ratio", "Temperature_C", "Humidity_percent",
            "Daily_feed_per_pig_g", "Feed_adjust_percent", "Meal_target_g",
            "Empty_meals_last_10", "Leftover_meals_last_10", "Tray_Load_G"]
TARGET = "Dispense_G"

df = pd.read_csv("SMARTPIG_8_Adaptive_Dataset.csv")
df = df[df["Pig_count"] == 3].reset_index(drop=True)        # the system has exactly 3 pigs
X, y = df[FEATURES], df[TARGET]

model = RandomForestRegressor(n_estimators=10, max_depth=6, min_samples_leaf=10, random_state=42)
pred = cross_val_predict(model, X, y, groups=df["Age_days"], cv=GroupKFold(n_splits=5)).clip(0)
rule = (df["Meal_target_g"] - df["Tray_Load_G"]).clip(lower=0)
print("Random forest MAE: %.1f g   | exact rule MAE: %.1f g" % (mean_absolute_error(y, pred), mean_absolute_error(y, rule)))

model.fit(X, y)
code = port(model)
open("RandomForestRegressor.h", "w").write(code)
print("Wrote RandomForestRegressor.h (%d KB)" % (len(code) // 1024))
