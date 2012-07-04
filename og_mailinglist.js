if (Drupal.jsEnabled) {
  $(document).ready(function() {
    $(".toggle-quoted-text").click( function() {
      $(this).parent().next().toggle();
      if ($(this).parent().next().css('display') == "block") {
        $(this).text(Drupal.t('-Hide quoted text-'));
      }
      else {
        $(this).text(Drupal.t('-Show quoted text-'));
      }
    });
  });
}
