import { ChangeDetectionStrategy, Component, inject } from '@angular/core';
import { AlertsService } from '../../../core/services/alerts.service';

@Component({
  selector: 'hd-alert-overlay',
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    @if (alerts.alertaCritico(); as a) {
      <div class="modal" role="alertdialog" aria-modal="true" aria-live="assertive">
        <div class="caixa critical">
          <div class="cabecalho">
            <span class="sev mono">CRÍTICO</span>
            <span class="titulo">{{ a.titulo }}</span>
          </div>
          <p class="msg">{{ a.mensagem }}</p>
          <button class="btn" (click)="alerts.confirmar(a.id)">CONFIRMAR</button>
        </div>
      </div>
    }

    <div class="toast-stack" aria-live="polite">
      @for (a of alerts.alertas(); track a.id) {
        @if (!a.requerConfirmacao) {
          <div class="toast" [class]="a.severidade">
            <span class="sev mono">{{ a.severidade.toUpperCase() }}</span>
            <div class="corpo">
              <div class="titulo">{{ a.titulo }}</div>
              <div class="msg">{{ a.mensagem }}</div>
            </div>
            <button class="fechar" (click)="alerts.confirmar(a.id)" aria-label="Fechar">×</button>
          </div>
        }
      }
    </div>
  `,
  styleUrl: './alert-overlay.component.scss',
})
export class AlertOverlayComponent {
  readonly alerts = inject(AlertsService);
}
