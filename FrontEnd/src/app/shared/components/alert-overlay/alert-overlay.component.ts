import { CUSTOM_ELEMENTS_SCHEMA, ChangeDetectionStrategy, Component, inject } from '@angular/core';
import { Severidade } from '../../../core/models/alerta.model';
import { AlertsService } from '../../../core/services/alerts.service';

const ICONE_SEVERIDADE: Record<Severidade, string> = {
  critical: 'ph:warning-octagon-fill',
  warning:  'ph:warning-diamond-fill',
  info:     'ph:info-fill',
  ok:       'ph:check-circle-fill',
};

@Component({
  selector: 'hd-alert-overlay',
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  schemas: [CUSTOM_ELEMENTS_SCHEMA],
  template: `
    @if (alerts.alertaCritico(); as a) {
      <div class="modal" role="alertdialog" aria-modal="true" aria-live="assertive">
        <div class="caixa critical">
          <iconify-icon class="sev-ico" [attr.icon]="iconeDe('critical')" aria-hidden="true"></iconify-icon>
          <div class="cabecalho">
            <span class="sev mono">CRÍTICO</span>
            <span class="titulo">{{ a.titulo }}</span>
          </div>
          <p class="msg">{{ a.mensagem }}</p>
          <button class="btn" (click)="alerts.confirmar(a.id)">
            <iconify-icon icon="ph:check-bold" aria-hidden="true"></iconify-icon>
            CONFIRMAR
          </button>
        </div>
      </div>
    }

    <div class="toast-stack" aria-live="polite">
      @for (a of alerts.alertas(); track a.id) {
        @if (!a.requerConfirmacao) {
          <div class="toast" [class]="a.severidade">
            <iconify-icon class="sev-ico" [attr.icon]="iconeDe(a.severidade)" aria-hidden="true"></iconify-icon>
            <div class="corpo">
              <div class="titulo">{{ a.titulo }}</div>
              <div class="msg">{{ a.mensagem }}</div>
            </div>
            <button class="fechar" (click)="alerts.confirmar(a.id)" aria-label="Fechar">
              <iconify-icon icon="ph:x-bold"></iconify-icon>
            </button>
          </div>
        }
      }
    </div>
  `,
  styleUrl: './alert-overlay.component.scss',
})
export class AlertOverlayComponent {
  readonly alerts = inject(AlertsService);

  iconeDe(sev: Severidade): string {
    return ICONE_SEVERIDADE[sev];
  }
}
