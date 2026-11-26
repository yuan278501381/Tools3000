import { describe, expect, it } from 'vitest';
import { isBuiltinPluginId } from './routes';
import { normalizeProcessInput } from '../utils/processName';

describe('Builtin Plugin SSOT & Hotkey Exclusion Normalization', () => {
  it('treats quick_look and all built-in modules as built-in plugins so they never appear as phantom sidebar extensions', () => {
    expect(isBuiltinPluginId('quick_look')).toBe(true);
    expect(isBuiltinPluginId('search')).toBe(true);
    expect(isBuiltinPluginId('spotlight')).toBe(true);
    expect(isBuiltinPluginId('capture')).toBe(true);
    expect(isBuiltinPluginId('third_party_demo_ext')).toBe(false);
  });

  it('normalizes process names from raw names, paths, and mixed casing consistently', () => {
    expect(normalizeProcessInput('  SSMS  ')).toBe('ssms.exe');
    expect(normalizeProcessInput('C:\\Program Files\\SSMS\\Ssms.EXE')).toBe('ssms.exe');
    expect(normalizeProcessInput('emacs.exe')).toBe('emacs.exe');
    expect(normalizeProcessInput('   ')).toBe('');
  });
});
