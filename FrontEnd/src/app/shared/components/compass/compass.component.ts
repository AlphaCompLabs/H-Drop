import { ChangeDetectionStrategy, Component, input } from '@angular/core';

@Component({
  selector: 'hd-compass',
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <div class="compass" role="img" [attr.aria-label]="'Proa ' + proa() + ' graus'">
      <span class="label n">N</span>
      <span class="label s">S</span>
      <span class="label e">E</span>
      <span class="label o">O</span>
      <div class="agulha" [style.transform]="'translate(-50%,-100%) rotate(' + proa() + 'deg)'">
        <div class="norte"></div>
        <div class="sul"></div>
      </div>
      <div class="pivot"></div>
    </div>
    <div class="valor mono">{{ proa().toFixed(1) }}<span class="grau">°</span></div>
  `,
  styleUrl: './compass.component.scss',
})
export class CompassComponent {
  readonly proa = input.required<number>();
}
