import { ChangeDetectionStrategy, Component, input } from '@angular/core';

export type CardTone = 'default' | 'ok' | 'warning' | 'critical' | 'info';

@Component({
  selector: 'hd-telemetry-card',
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <div class="card" [class]="tone()" [class.destaque]="destaque()">
      <div class="label mono">{{ label() }}</div>
      <div class="valor mono">
        {{ valor() }}@if (unidade()) {<span class="unidade">{{ unidade() }}</span>}
      </div>
    </div>
  `,
  styleUrl: './telemetry-card.component.scss',
})
export class TelemetryCardComponent {
  readonly label = input.required<string>();
  readonly valor = input<string | number>('---');
  readonly unidade = input<string>('');
  readonly tone = input<CardTone>('default');
  readonly destaque = input<boolean>(false);
}
