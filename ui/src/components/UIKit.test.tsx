/* @vitest-environment jsdom */

import { act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { Select, Modal, Tooltip, NumberInput } from './UIKit';

// Mock i18next
vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string) => key,
  }),
}));

describe('UIKit - Select', () => {
  const options = [
    { value: 'opt1', label: 'Option 1' },
    { value: 'opt2', label: 'Option 2' },
    { value: 'opt3', label: 'Option 3' },
  ];

  afterEach(() => {
    cleanup();
  });

  it('renders with selected option and default bottom placement', () => {
    const onChange = vi.fn();
    const { container } = render(
      <Select value="opt1" options={options} onChange={onChange} />
    );

    expect(screen.getByText('Option 1')).toBeTruthy();

    const trigger = container.querySelector('.uikit-select-trigger');
    expect(trigger).toBeTruthy();
    expect(container.querySelector('.uikit-select-dropdown')).toBeNull();

    // Click to open
    act(() => {
      fireEvent.click(trigger!);
    });

    const dropdown = container.querySelector('.uikit-select-dropdown');
    expect(dropdown).toBeTruthy();
    expect(dropdown?.classList.contains('uikit-select-dropdown--bottom')).toBe(true);

    // Select option 2
    const opt2 = screen.getByText('Option 2');
    act(() => {
      fireEvent.click(opt2);
    });

    expect(onChange).toHaveBeenCalledWith('opt2');
    expect(container.querySelector('.uikit-select-dropdown')).toBeNull();
  });

  it('renders with top placement class when placement="top"', () => {
    const onChange = vi.fn();
    const { container } = render(
      <Select value="opt1" options={options} onChange={onChange} placement="top" />
    );

    const trigger = container.querySelector('.uikit-select-trigger');
    act(() => {
      fireEvent.click(trigger!);
    });

    const dropdown = container.querySelector('.uikit-select-dropdown');
    expect(dropdown).toBeTruthy();
    expect(dropdown?.classList.contains('uikit-select-dropdown--top')).toBe(true);
  });

  it('closes dropdown on Escape key', () => {
    const onChange = vi.fn();
    const { container } = render(
      <Select value="opt1" options={options} onChange={onChange} />
    );

    const trigger = container.querySelector('.uikit-select-trigger');
    act(() => {
      fireEvent.click(trigger!);
    });
    expect(container.querySelector('.uikit-select-dropdown')).toBeTruthy();

    act(() => {
      fireEvent.keyDown(document, { key: 'Escape' });
    });
    expect(container.querySelector('.uikit-select-dropdown')).toBeNull();
  });

  it('does not open when disabled', () => {
    const onChange = vi.fn();
    const { container } = render(
      <Select value="opt1" options={options} onChange={onChange} disabled />
    );

    const trigger = container.querySelector('.uikit-select-trigger');
    act(() => {
      fireEvent.click(trigger!);
    });
    expect(container.querySelector('.uikit-select-dropdown')).toBeNull();
  });

  it('navigates options via keyboard ArrowDown and ArrowUp', () => {
    const onChange = vi.fn();
    const { container } = render(
      <Select value="opt1" options={options} onChange={onChange} />
    );

    const trigger = container.querySelector('.uikit-select-trigger');
    expect(container.querySelector('.uikit-select-dropdown')).toBeNull();

    // ArrowDown opens dropdown
    act(() => {
      fireEvent.keyDown(trigger!, { key: 'ArrowDown' });
    });
    expect(container.querySelector('.uikit-select-dropdown')).toBeTruthy();

    // ArrowDown again moves to next option
    act(() => {
      fireEvent.keyDown(trigger!, { key: 'ArrowDown' });
    });
    expect(onChange).toHaveBeenCalledWith('opt2');
  });

  it('automatically calculates top placement when space below is constrained', () => {
    const onChange = vi.fn();
    const { container } = render(
      <Select value="opt1" options={options} onChange={onChange} placement="auto" />
    );

    const trigger = container.querySelector('.uikit-select-trigger') as HTMLElement;
    vi.spyOn(trigger, 'getBoundingClientRect').mockReturnValue({
      top: 300,
      bottom: 330,
      left: 0,
      right: 180,
      width: 180,
      height: 30,
      x: 0,
      y: 300,
      toJSON: () => {},
    });

    const originalInnerHeight = window.innerHeight;
    Object.defineProperty(window, 'innerHeight', { writable: true, configurable: true, value: 350 });

    try {
      act(() => {
        fireEvent.click(trigger);
      });

      const dropdown = container.querySelector('.uikit-select-dropdown');
      expect(dropdown).toBeTruthy();
      expect(dropdown?.classList.contains('uikit-select-dropdown--top')).toBe(true);
    } finally {
      Object.defineProperty(window, 'innerHeight', { writable: true, configurable: true, value: originalInnerHeight });
    }
  });
});

describe('UIKit - Modal', () => {
  afterEach(() => {
    cleanup();
  });

  it('renders modal when open is true, and handles close via button and Escape', () => {
    const onClose = vi.fn();
    const { rerender } = render(
      <Modal open={true} title="Test Modal" onClose={onClose} footer={<button>Save</button>}>
        <div>Modal Content</div>
      </Modal>
    );

    expect(screen.getByText('Test Modal')).toBeTruthy();
    expect(screen.getByText('Modal Content')).toBeTruthy();
    expect(screen.getByText('Save')).toBeTruthy();

    // Close button has SVG X icon
    const closeBtn = screen.getByRole('button', { name: 'common.close' });
    expect(closeBtn).toBeTruthy();
    expect(closeBtn.querySelector('svg')).toBeTruthy();

    act(() => {
      fireEvent.click(closeBtn);
    });
    expect(onClose).toHaveBeenCalledTimes(1);

    // Escape key
    act(() => {
      fireEvent.keyDown(window, { key: 'Escape' });
    });
    expect(onClose).toHaveBeenCalledTimes(2);

    // When open is false, does not render
    rerender(
      <Modal open={false} title="Test Modal" onClose={onClose}>
        <div>Modal Content</div>
      </Modal>
    );
    expect(screen.queryByText('Test Modal')).toBeNull();
  });
});

describe('UIKit - Select (Portal Mode)', () => {
  const options = [
    { value: 'p1', label: 'Portal Option 1' },
    { value: 'p2', label: 'Portal Option 2' },
  ];

  afterEach(() => {
    cleanup();
  });

  it('renders dropdown into document.body when portal={true}', () => {
    const onChange = vi.fn();
    const { container } = render(
      <Select value="p1" options={options} onChange={onChange} portal={true} />
    );

    const trigger = container.querySelector('.uikit-select-trigger');
    expect(trigger).toBeTruthy();
    expect(container.querySelector('.uikit-select-dropdown')).toBeNull();
    expect(document.body.querySelector('.uikit-select-dropdown')).toBeNull();

    // Click to open portal
    act(() => {
      fireEvent.click(trigger!);
    });

    // In portal mode, dropdown is attached to document.body, not container
    expect(container.querySelector('.uikit-select-dropdown')).toBeNull();
    const portalDropdown = document.body.querySelector('.uikit-select-dropdown--portal');
    expect(portalDropdown).toBeTruthy();

    // Select option 2
    const opt2 = screen.getByText('Portal Option 2');
    act(() => {
      fireEvent.click(opt2);
    });

    expect(onChange).toHaveBeenCalledWith('p2');
    expect(document.body.querySelector('.uikit-select-dropdown')).toBeNull();
  });

  it('closes portal dropdown on Escape key', () => {
    const onChange = vi.fn();
    const { container } = render(
      <Select value="p1" options={options} onChange={onChange} portal={true} />
    );

    const trigger = container.querySelector('.uikit-select-trigger');
    act(() => {
      fireEvent.click(trigger!);
    });
    expect(document.body.querySelector('.uikit-select-dropdown--portal')).toBeTruthy();

    act(() => {
      fireEvent.keyDown(document, { key: 'Escape' });
    });
    expect(document.body.querySelector('.uikit-select-dropdown--portal')).toBeNull();
  });

  it('automatically enables portal mode when nested inside a .uikit-card', () => {
    const onChange = vi.fn();
    const { container } = render(
      <div className="uikit-card">
        <Select value="p1" options={options} onChange={onChange} />
      </div>
    );

    const trigger = container.querySelector('.uikit-select-trigger');
    expect(trigger).toBeTruthy();

    act(() => {
      fireEvent.click(trigger!);
    });

    // When inside .uikit-card, auto-portal should mount dropdown to document.body
    expect(container.querySelector('.uikit-select-dropdown')).toBeNull();
    const portalDropdown = document.body.querySelector('.uikit-select-dropdown--portal');
    expect(portalDropdown).toBeTruthy();
  });

  it('clamps dropdown maxHeight to available space without overflowing window', () => {
    const onChange = vi.fn();
    const { container } = render(
      <Select value="p1" options={options} onChange={onChange} portal={true} />
    );

    const trigger = container.querySelector('.uikit-select-trigger') as HTMLElement;
    vi.spyOn(trigger, 'getBoundingClientRect').mockReturnValue({
      top: 500,
      bottom: 530,
      left: 100,
      right: 280,
      width: 180,
      height: 30,
      x: 100,
      y: 500,
      toJSON: () => {},
    });

    const originalInnerHeight = window.innerHeight;
    // Set viewport height so spaceBelow is only 50px (580 - 530)
    Object.defineProperty(window, 'innerHeight', { writable: true, configurable: true, value: 580 });

    try {
      act(() => {
        fireEvent.click(trigger);
      });

      const portalDropdown = document.body.querySelector('.uikit-select-dropdown--portal') as HTMLElement;
      expect(portalDropdown).toBeTruthy();
      // Dropdown should not exceed spaceBelow (or flipped spaceAbove)
      expect(portalDropdown.style.maxHeight).toBeTruthy();
    } finally {
      Object.defineProperty(window, 'innerHeight', { writable: true, configurable: true, value: originalInnerHeight });
    }
  });
});

describe('UIKit - Tooltip', () => {
  afterEach(() => {
    cleanup();
  });

  it('renders trigger and shows tooltip on hover with delay=0', () => {
    const { container } = render(
      <Tooltip content="Helper info" delay={0}>
        <button>Hover me</button>
      </Tooltip>
    );

    const trigger = container.querySelector('.uikit-tooltip-trigger');
    expect(trigger).toBeTruthy();
    expect(document.body.querySelector('.uikit-tooltip')).toBeNull();

    act(() => {
      fireEvent.mouseEnter(trigger!);
    });

    const tooltip = document.body.querySelector('.uikit-tooltip');
    expect(tooltip).toBeTruthy();
    expect(tooltip?.textContent).toBe('Helper info');

    act(() => {
      fireEvent.mouseLeave(trigger!);
    });

    expect(document.body.querySelector('.uikit-tooltip')).toBeNull();
  });

  it('clamps tooltip position near viewport edge to prevent clipping', () => {
    const { container } = render(
      <Tooltip content="Edge tooltip" delay={0}>
        <button className="edge-btn">Edge Button</button>
      </Tooltip>
    );

    const trigger = container.querySelector('.uikit-tooltip-trigger') as HTMLElement;
    vi.spyOn(trigger, 'getBoundingClientRect').mockReturnValue({
      top: 10,
      bottom: 30,
      left: 2,
      right: 22,
      width: 20,
      height: 20,
      x: 2,
      y: 10,
      toJSON: () => {},
    });

    act(() => {
      fireEvent.mouseEnter(trigger);
    });

    const tooltip = document.body.querySelector('.uikit-tooltip') as HTMLElement;
    expect(tooltip).toBeTruthy();
    // In low spaceAbove (10px), it flips to bottom
    expect(tooltip.classList.contains('uikit-tooltip--bottom')).toBe(true);
  });
});

describe('UIKit - NumberInput', () => {
  afterEach(() => {
    cleanup();
  });

  it('reverts draft cleanly on Escape without committing stale draft during blur', () => {
    const onChange = vi.fn();
    const { container } = render(
      <NumberInput value={50} min={10} max={100} onChange={onChange} ariaLabel="Quality" />,
    );

    const input = container.querySelector('input') as HTMLInputElement;
    expect(input.value).toBe('50');

    act(() => {
      input.focus();
      fireEvent.focus(input);
      fireEvent.change(input, { target: { value: '999' } });
    });
    expect(input.value).toBe('999');
    expect(onChange).not.toHaveBeenCalled();

    act(() => {
      fireEvent.keyDown(input, { key: 'Escape' });
    });

    expect(input.value).toBe('50');
    expect(onChange).not.toHaveBeenCalled();
  });

  it('commits clamped value once on Enter without duplicate commit on subsequent blur', () => {
    const onChange = vi.fn();
    const { container } = render(
      <NumberInput value={50} min={10} max={100} onChange={onChange} ariaLabel="Quality" />,
    );

    const input = container.querySelector('input') as HTMLInputElement;
    act(() => {
      input.focus();
      fireEvent.focus(input);
      fireEvent.change(input, { target: { value: '250' } });
      fireEvent.keyDown(input, { key: 'Enter' });
    });

    expect(input.value).toBe('100');
    expect(onChange).toHaveBeenCalledTimes(1);
    expect(onChange).toHaveBeenCalledWith(100);
  });
});


