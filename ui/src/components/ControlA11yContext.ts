import { createContext, useContext } from 'react';

export interface ControlA11yContextValue {
  labelledBy?: string;
  describedBy?: string;
}

export const ControlA11yContext = createContext<ControlA11yContextValue>({});
export const useControlA11y = () => useContext(ControlA11yContext);
