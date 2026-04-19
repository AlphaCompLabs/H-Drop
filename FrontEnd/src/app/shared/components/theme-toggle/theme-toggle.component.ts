import { ChangeDetectionStrategy, Component, inject } from '@angular/core';
import { ThemeService } from '../../../core/services/theme.service';

@Component({
  selector: 'hd-theme-toggle',
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <button
      class="toggle"
      type="button"
      (click)="theme.alternar()"
      [attr.aria-label]="theme.tema() === 'dark' ? 'Mudar para tema claro' : 'Mudar para tema escuro'"
      [attr.title]="theme.tema() === 'dark' ? 'Mission Dark' : 'Utility Light'">
      <span class="mono">{{ theme.tema() === 'dark' ? '☀ LIGHT' : '☽ DARK' }}</span>
    </button>
  `,
  styles: [`
    .toggle {
      padding: 4px 12px;
      border: 1px solid var(--c-border);
      border-radius: var(--r-sm);
      color: var(--c-text-dim);
      font-size: 0.7rem;
      letter-spacing: 2px;
      transition: all var(--t-fast);
    }
    .toggle:hover { color: var(--c-accent); border-color: var(--c-accent); }
  `],
})
export class ThemeToggleComponent {
  readonly theme = inject(ThemeService);
}
