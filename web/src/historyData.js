import { useEffect, useState } from "react";
import { subscribeToHistory } from "./dataService";

export const RECORD_COUNT = 60;

// A history value counts only when the sensor said the reading was good
export function validNumber(row, field) {
  return row.sensorOk === true && typeof row[field] === "number" ? row[field] : null;
}

// Last N history records, oldest first
export function useHistory(count = RECORD_COUNT) {
  const [rows, setRows] = useState([]);
  const [hasLoaded, setHasLoaded] = useState(false);
  const [error, setError] = useState(null);

  useEffect(() => {
    return subscribeToHistory(
      count,
      (data) => {
        setRows(data);
        setError(null);
        setHasLoaded(true);
      },
      (err) => {
        setError(err.message);
        setHasLoaded(true);
      }
    );
  }, [count]);

  return { rows, hasLoaded, error };
}
